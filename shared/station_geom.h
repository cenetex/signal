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
    /* Worst-case corridor count: per ring, (modules - 1) corridors
     * since rings are intentionally always open. Sum of per-ring
     * (slots-1) = 2 + 5 + 8 = 15. Round up for headroom. */
    STATION_GEOM_MAX_CORRIDORS = 18,
    /* Spokes connect a producer to its cross-ring paired hopper.
     * Up to one spoke per producer; cap at the module limit. */
    STATION_GEOM_MAX_SPOKES    = MAX_MODULES_PER_STATION,
    STATION_GEOM_MAX_DOCKS     = 6,
};

typedef struct {
    vec2  center;
    float radius;
    int   ring;
    float angle;          /* angular position relative to station center */
    bool  is_scaffold;    /* true = under construction */
    float build_progress; /* 0..1 material delivery, 1..2 construction timer */
} geom_circle_t;

typedef struct {
    /* angle_a is the LOWER-slot endpoint, angle_b is the HIGHER-slot
     * endpoint (with ring rotation baked in). arc_delta is the
     * canonical FORWARD span between them, in radians, always > 0
     * and always < 2π. Consumers MUST use arc_delta directly rather
     * than recomputing wrap_angle(angle_b - angle_a) — for sort-
     * adjacent slots that span more than 180° (e.g. slot 0 → slot 5
     * on a 6-slot ring with intermediate slots empty), the
     * shortest-arc normalization picks the wrong direction. */
    float angle_a;
    float angle_b;
    float arc_delta;
    float ring_radius;
    int   ring;
} geom_corridor_t;

/* Cross-ring spoke: a tractor beam connecting a producer to its
 * paired intake (HOPPER) on an adjacent ring. Purely a render hint —
 * spokes do NOT contribute to collision (ships fly through them). */
typedef struct {
    vec2 a;       /* producer module world position */
    vec2 b;       /* paired intake module world position */
    int  ring_a;  /* producer ring */
    int  ring_b;  /* intake ring (adjacent to ring_a) */
} geom_spoke_t;

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

    /* Corridor arcs — same-ring connectors. Rings are always OPEN:
     * the renderer never closes the wrap-around edge, so visual gap
     * is always present and the corridor count tops out at
     * (modules_on_ring - 1) per ring. */
    geom_corridor_t corridors[STATION_GEOM_MAX_CORRIDORS];
    int corridor_count;

    /* Cross-ring spokes — render-only tractor beams between a
     * producer and its paired hopper. */
    geom_spoke_t spokes[STATION_GEOM_MAX_SPOKES];
    int spoke_count;

    /* Dock positions (for rendering berths, not collision) */
    geom_dock_t docks[STATION_GEOM_MAX_DOCKS];
    int dock_count;
} station_geom_t;

/*
 * Build the collision/render geometry for a station.
 * Caller provides a stack-allocated station_geom_t.
 *
 * Emitted (in order):
 *   - Core collision circle (if station has radius).
 *   - One module circle per non-dock module; smaller circle for docks.
 *   - Corridor arcs connecting every sort-adjacent pair of modules
 *     on the same ring. Rings are ALWAYS OPEN — the wrap-around
 *     closing edge is never emitted, so the visual gap matches the
 *     largest empty span between sort-adjacent modules. Each corridor
 *     carries an explicit `arc_delta` in [0, 2π) that equals the
 *     forward (slot-increasing) span; consumers must NOT recompute
 *     it via wrap_angle (that picks the shortest arc, which is
 *     wrong when modules are sort-adjacent across a wide gap).
 *   - Cross-ring spokes from each producer to its paired intake
 *     module (typically HOPPER) on an adjacent ring, when the pair
 *     is satisfied. Spokes are render-only — they do not contribute
 *     to collision.
 *   - Dock world positions for berth rendering.
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
                /* Dock gets a smaller collision circle — narrow passage, not wide hole */
                if (out->circle_count < STATION_GEOM_MAX_CIRCLES) {
                    geom_circle_t *c = &out->circles[out->circle_count++];
                    c->center = mpos;
                    c->radius = STATION_MODULE_COL_RADIUS * 0.5f;
                    c->ring = ring;
                    c->angle = mang;
                    c->is_scaffold = mod->scaffold;
                    c->build_progress = mod->build_progress;
                }
            } else {
                if (out->circle_count < STATION_GEOM_MAX_CIRCLES) {
                    geom_circle_t *c = &out->circles[out->circle_count++];
                    c->center = mpos;
                    c->radius = STATION_MODULE_COL_RADIUS;
                    c->ring = ring;
                    c->angle = mang;
                    c->is_scaffold = mod->scaffold;
                    c->build_progress = mod->build_progress;
                }
            }
        }

        /* Corridors: connect every sort-adjacent module pair on the
         * ring. The slot-difference is encoded directly in arc_delta
         * so consumers can drive arc rendering / collision without
         * recomputing it (wrap_angle picks the wrong direction when
         * sort-adjacent slots span > 180°). Rings stay open — no
         * wrap-around edge — so the visual gap is always the largest
         * empty arc on the ring. */
        for (int ci = 0; ci + 1 < count; ci++) {
            uint8_t slot_a = st->modules[idx[ci]].slot;
            uint8_t slot_b = st->modules[idx[ci+1]].slot;
            if (out->corridor_count >= STATION_GEOM_MAX_CORRIDORS) break;
            geom_corridor_t *cor = &out->corridors[out->corridor_count++];
            cor->angle_a = module_angle_ring(st, ring, slot_a);
            cor->angle_b = module_angle_ring(st, ring, slot_b);
            cor->arc_delta = TWO_PI_F * (float)(slot_b - slot_a) / (float)slots;
            cor->ring_radius = ring_r;
            cor->ring = ring;
        }
    }

    /* Cross-ring spokes — render-only tractor beams from each
     * producer to its paired intake module. The pair is satisfied
     * when station_pair_neighbors returns a slot on an adjacent ring
     * holding the producer's required pair_intake type. Skip
     * scaffolded modules on either end — partial structures don't
     * draw a beam yet. */
    for (int m = 0; m < st->module_count; m++) {
        const station_module_t *prod = &st->modules[m];
        if (prod->scaffold) continue;
        if (!module_requires_pair(prod->type)) continue;
        module_type_t need = module_pair_intake(prod->type);
        station_slot_pair_t cand[2];
        int n = station_pair_neighbors((int)prod->ring, (int)prod->slot, cand);
        for (int c = 0; c < n; c++) {
            if (station_module_at(st, cand[c].ring, cand[c].slot) != need) continue;
            if (out->spoke_count >= STATION_GEOM_MAX_SPOKES) break;
            geom_spoke_t *sp = &out->spokes[out->spoke_count++];
            sp->a = module_world_pos_ring(st, prod->ring, prod->slot);
            sp->b = module_world_pos_ring(st, cand[c].ring, cand[c].slot);
            sp->ring_a = prod->ring;
            sp->ring_b = cand[c].ring;
            break; /* one spoke per producer — first satisfied neighbor */
        }
    }
}

#endif /* STATION_GEOM_H */
