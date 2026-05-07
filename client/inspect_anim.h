/*
 * inspect_anim.h -- Per-row content fingerprint for the inspect-pane
 * scramble-resolve animation. The animation re-fires only when the
 * fingerprint changes; identical-content frames skip the settle.
 *
 * Defined static inline so signal_test (which doesn't link sokol / the
 * rest of hud.c) can call it without pulling in HUD deps.
 */
#ifndef INSPECT_ANIM_H
#define INSPECT_ANIM_H

#include <stdint.h>
#include "net.h"  /* NetInspectSnapshotRow */

static inline uint64_t hud_row_signature(const NetInspectSnapshotRow *row) {
    /* FNV-1a over the fields whose change means a different row.
     * Volatile presentation-time data only — no allocation, no hashing
     * library, deterministic across builds. */
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *bytes[] = {
        row->cargo_pub, row->receipt_head,
        row->origin_station, row->latest_station,
    };
    for (size_t b = 0; b < sizeof(bytes)/sizeof(bytes[0]); b++) {
        for (int i = 0; i < 32; i++) {
            h ^= bytes[b][i];
            h *= 0x100000001b3ull;
        }
    }
    uint8_t scalars[] = {
        row->commodity, row->grade, row->chain_len, row->flags,
        (uint8_t)(row->quantity & 0xff),
        (uint8_t)((row->quantity >> 8) & 0xff),
        (uint8_t)((row->event_id      ) & 0xff),
        (uint8_t)((row->event_id >>  8) & 0xff),
        (uint8_t)((row->event_id >> 16) & 0xff),
        (uint8_t)((row->event_id >> 24) & 0xff),
        (uint8_t)((row->event_id >> 32) & 0xff),
        (uint8_t)((row->event_id >> 40) & 0xff),
        (uint8_t)((row->event_id >> 48) & 0xff),
        (uint8_t)((row->event_id >> 56) & 0xff),
    };
    for (size_t i = 0; i < sizeof(scalars); i++) {
        h ^= scalars[i];
        h *= 0x100000001b3ull;
    }
    /* The sig == 0 sentinel marks an unused slot in the per-row anim
     * state; force any real row away from that value. */
    if (h == 0) h = 1;
    return h;
}

#endif
