/*
 * station_palette.h — render-time palette helpers for station modules.
 *
 * These helpers let the renderer pick a module color based on station
 * context (e.g. how many furnaces a station has, which ring this furnace
 * sits on) without bumping the schema or save format. The simulation
 * remains unaware: it only knows the generic MODULE_FURNACE type.
 *
 * Test-friendly: defined as a static inline so test_furnace_color.c can
 * include this header and exercise the same logic the renderer uses.
 */
#ifndef SIGNAL_STATION_PALETTE_H
#define SIGNAL_STATION_PALETTE_H

#include "types.h"
#include "palette.h"

/* Pick the per-ring furnace tint for a single MODULE_FURNACE on `st`,
 * sitting at ring `my_ring`. The matrix:
 *
 *   1 furnace:    inner ring → ferrite (red)
 *   2 furnaces:   inner → ferrite, outer → cuprite (blue)
 *   3+ furnaces:  innermost → crystal (green),
 *                 outermost → cuprite (blue),
 *                 anything in between → chunks-feeder (white)
 *
 * Rationale: 1-furnace stations (Prospect) are pure ferrite smelters;
 * 3+-furnace stations (Helios) specialize in cuprite + crystal with a
 * middle "chunks-feeder" helper. Player-planted outposts that grow
 * 1 → 2 → 3 furnaces will retint automatically as new furnaces are
 * added — the table is read at render time from station state. */
static inline void station_palette_furnace_color(const station_t *st,
                                                 int my_ring,
                                                 float *r, float *g, float *b) {
    int n = 0;
    int min_ring = 99, max_ring = 0;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_FURNACE) continue;
        n++;
        int rr = (int)st->modules[i].ring;
        if (rr < min_ring) min_ring = rr;
        if (rr > max_ring) max_ring = rr;
    }
    if (n <= 1) {
        PAL_UNPACK3(PAL_FURNACE_FERRITE, *r, *g, *b);
        return;
    }
    if (n == 2) {
        if (my_ring == min_ring) PAL_UNPACK3(PAL_FURNACE_FERRITE, *r, *g, *b);
        else                     PAL_UNPACK3(PAL_FURNACE_CUPRITE, *r, *g, *b);
        return;
    }
    /* 3+ furnaces: inner=crystal, outer=cuprite, middle=chunks. */
    if (my_ring == min_ring) {
        PAL_UNPACK3(PAL_FURNACE_CRYSTAL, *r, *g, *b);
    } else if (my_ring == max_ring) {
        PAL_UNPACK3(PAL_FURNACE_CUPRITE, *r, *g, *b);
    } else {
        /* TODO(#chunks-feeder): sim behavior — middle-ring furnace
         * tractors fragments inward to feed outer smelters. Today this
         * is render-only; the simulation treats it as a normal furnace. */
        PAL_UNPACK3(PAL_FURNACE_CHUNKS, *r, *g, *b);
    }
}

#endif /* SIGNAL_STATION_PALETTE_H */
