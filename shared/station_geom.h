/*
 * station_geom.h — Unified station collision/render geometry emitter.
 *
 * One function (station_build_geom) produces all collision shapes for a
 * station. All consumers — player collision, NPC collision, asteroid
 * collision, and rendering — read from this shared geometry.
 *
 * Constants STATION_CORRIDOR_HW and STATION_MODULE_COL_RADIUS are the
 * single source of truth for corridor half-width and module collision
 * radius. No duplicates allowed.
 */
#ifndef STATION_GEOM_H
#define STATION_GEOM_H

#include <string.h>
/* types.h includes this file at the bottom — don't re-include types.h.
 * All types (station_t, vec2, etc.) are available from the includer. */

/* Collision constants — single source of truth */
#define STATION_CORRIDOR_HW      10.0f   /* corridor half-width (radial band ± this from ring_r) */
#define STATION_MODULE_COL_RADIUS 34.0f  /* module collision circle radius (1.4x scaled half-size) */

enum {
    STATION_GEOM_MAX_CIRCLES   = MAX_MODULES_PER_STATION,
    STATION_GEOM_MAX_CORRIDORS = 18,  /* worst case: 3 + 6 + 9 */
    STATION_GEOM_MAX_DOCKS     = 6,
};

typedef struct {
    vec2  center;
    float radius;
    int   ring;
    float angle;    /* angular position relative to station center */
} geom_circle_t;

typedef struct {
    float angle_a;
    float angle_b;
    float ring_radius;
    int   ring;
} geom_corridor_t;

typedef struct {
    vec2  pos;
    float angle;
    int   ring;
    int   slot;
} geom_dock_t;

typedef struct {
    /* Station center (copied for convenience) */
    vec2 center;

    /* Core collision circle */
    geom_circle_t core;
    bool has_core;

    /* Module collision circles (docks excluded — docks are passages) */
    geom_circle_t circles[STATION_GEOM_MAX_CIRCLES];
    int circle_count;

    /* Corridor arcs (dock gaps already resolved) */
    geom_corridor_t corridors[STATION_GEOM_MAX_CORRIDORS];
    int corridor_count;

    /* Dock positions (for rendering berths, not collision) */
    geom_dock_t docks[STATION_GEOM_MAX_DOCKS];
    int dock_count;
} station_geom_t;

/*
 * Build the collision/render geometry for a station.
 * Caller provides a stack-allocated station_geom_t.
 * The emitter enumerates modules per ring, sorts by slot, emits:
 *   - core circle (if station has radius)
 *   - module circles for non-dock modules
 *   - corridor arcs between adjacent modules (dock as first = gap)
 *   - wrap-around corridor if ring is full (skip if last is dock)
 *   - dock positions for rendering
 */
static inline void station_build_geom(const station_t *st, station_geom_t *out) {
    memset(out, 0, sizeof(*out));
    out->center = st->pos;

    /* Core */
    if (st->radius > 0.0f) {
        out->has_core = true;
        out->core.center = st->pos;
        out->core.radius = st->radius + 4.0f;
        out->core.ring = 0;
        out->core.angle = 0.0f;
    }

    /* Per-ring: collect modules, sort by slot, emit shapes */
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        int idx[MAX_MODULES_PER_STATION];
        int count = 0;
        for (int m = 0; m < st->module_count; m++)
            if (st->modules[m].ring == ring) idx[count++] = m;
        if (count == 0) continue;

        /* Insertion sort by slot */
        for (int i = 1; i < count; i++) {
            int tmp = idx[i]; int j = i - 1;
            while (j >= 0 && st->modules[idx[j]].slot > st->modules[tmp].slot)
                { idx[j+1] = idx[j]; j--; }
            idx[j+1] = tmp;
        }

        int slots = STATION_RING_SLOTS[ring];
        float ring_r = STATION_RING_RADIUS[ring];

        /* Emit circles and dock positions */
        for (int ci = 0; ci < count; ci++) {
            const station_module_t *mod = &st->modules[idx[ci]];
            vec2 mpos = module_world_pos_ring(st, ring, mod->slot);
            float mang = module_angle_ring(st, ring, mod->slot);

            if (mod->type == MODULE_DOCK) {
                if (out->dock_count < STATION_GEOM_MAX_DOCKS) {
                    out->docks[out->dock_count].pos = mpos;
                    out->docks[out->dock_count].angle = mang;
                    out->docks[out->dock_count].ring = ring;
                    out->docks[out->dock_count].slot = mod->slot;
                    out->dock_count++;
                }
            } else {
                if (out->circle_count < STATION_GEOM_MAX_CIRCLES) {
                    out->circles[out->circle_count].center = mpos;
                    out->circles[out->circle_count].radius = STATION_MODULE_COL_RADIUS;
                    out->circles[out->circle_count].ring = ring;
                    out->circles[out->circle_count].angle = mang;
                    out->circle_count++;
                }
            }
        }

        /* Corridors: adjacent pairs, skip where dock is first */
        for (int ci = 0; ci + 1 < count; ci++) {
            if (st->modules[idx[ci+1]].slot - st->modules[idx[ci]].slot != 1) continue;
            if (st->modules[idx[ci]].type == MODULE_DOCK) continue;
            if (out->corridor_count < STATION_GEOM_MAX_CORRIDORS) {
                float a = module_angle_ring(st, ring, st->modules[idx[ci]].slot);
                float b = module_angle_ring(st, ring, st->modules[idx[ci+1]].slot);
                out->corridors[out->corridor_count].angle_a = a;
                out->corridors[out->corridor_count].angle_b = b;
                out->corridors[out->corridor_count].ring_radius = ring_r;
                out->corridors[out->corridor_count].ring = ring;
                out->corridor_count++;
            }
        }

        /* Wrap-around: last→first if ring is full and last is not dock */
        if (count == slots && st->modules[idx[count-1]].type != MODULE_DOCK) {
            if (out->corridor_count < STATION_GEOM_MAX_CORRIDORS) {
                float a = module_angle_ring(st, ring, st->modules[idx[count-1]].slot);
                float b = module_angle_ring(st, ring, st->modules[idx[0]].slot);
                out->corridors[out->corridor_count].angle_a = a;
                out->corridors[out->corridor_count].angle_b = b;
                out->corridors[out->corridor_count].ring_radius = ring_r;
                out->corridors[out->corridor_count].ring = ring;
                out->corridor_count++;
            }
        }
    }
}

#endif /* STATION_GEOM_H */
