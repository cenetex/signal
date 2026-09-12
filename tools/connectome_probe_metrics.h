/* Read-only outcome sampling for the headless connectome probe. */
#ifndef CONNECTOME_PROBE_METRICS_H
#define CONNECTOME_PROBE_METRICS_H
#include "game_sim.h"
#include <math.h>
#include <string.h>

typedef struct {
    uint16_t shipment_id[MAX_DELIVERY_SHIPMENTS];
    uint16_t delivered[MAX_DELIVERY_SHIPMENTS];
    uint32_t asset_id[MAX_SHIP_ASSETS];
    bool destroyed[MAX_SHIP_ASSETS];
    uint32_t npc_asset[MAX_NPC_SHIPS];
    bool npc_active[MAX_NPC_SHIPS];
    float hull[MAX_NPC_SHIPS];
    vec2 pos[MAX_NPC_SHIPS];
    uint64_t delivered_units, destroyed_ships;
    uint64_t active_ticks, travel_ticks, docked_ticks, idle_ticks, towing_ticks;
    uint64_t contract_completions, event_capacity_ticks;
    double distance, observed_hull_loss;
} connectome_probe_metrics_t;

static inline void connectome_probe_sample(connectome_probe_metrics_t *m,
                                            const world_t *w, bool initial)
{
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        const delivery_shipment_t *s = &w->delivery_shipments[i];
        if (!s->active) continue;
        uint16_t before = m->shipment_id[i] == s->shipment_id ? m->delivered[i] : 0;
        if (!initial && s->quantity_delivered > before)
            m->delivered_units += s->quantity_delivered - before;
        m->shipment_id[i] = s->shipment_id;
        m->delivered[i] = s->quantity_delivered;
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *a = &w->ship_assets[i];
        if (!a->active) continue;
        if (!initial && a->destroyed &&
            (m->asset_id[i] != a->asset_id || !m->destroyed[i])) m->destroyed_ships++;
        m->asset_id[i] = a->asset_id;
        m->destroyed[i] = a->destroyed;
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *n = &w->npc_ships[i];
        bool active = n->active && n->ship;
        bool same = active && m->npc_active[i] && n->ship_asset_id == m->npc_asset[i];
        if (!initial && same) {
            m->distance += sqrt((double)v2_dist_sq(n->ship->pos, m->pos[i]));
            if (m->hull[i] > n->ship->hull)
                m->observed_hull_loss += m->hull[i] - fmaxf(n->ship->hull, 0.0f);
        }
        if (!initial && !same && m->npc_active[i]) {
            const ship_asset_t *old = world_ship_asset_by_id_const(w, m->npc_asset[i]);
            if (old && old->destroyed) m->observed_hull_loss += fmaxf(m->hull[i], 0.0f);
        }
        if (!initial && active) {
            m->active_ticks++;
            if (n->state == NPC_STATE_TRAVEL_TO_ASTEROID ||
                n->state == NPC_STATE_TRAVEL_TO_DEST ||
                n->state == NPC_STATE_RETURN_TO_STATION) m->travel_ticks++;
            if (n->state == NPC_STATE_IDLE) m->idle_ticks++;
            if (n->state == NPC_STATE_DOCKED) m->docked_ticks++;
            if (n->ship->towed_count || n->ship->towed_pod_count ||
                n->ship->towed_scaffold >= 0) m->towing_ticks++;
        }
        m->npc_active[i] = active;
        if (active) {
            m->npc_asset[i] = n->ship_asset_id;
            m->hull[i] = n->ship->hull;
            m->pos[i] = n->ship->pos;
        }
    }
    if (!initial) {
        if (w->events.count == SIM_MAX_EVENTS) m->event_capacity_ticks++;
        for (int i = 0; i < w->events.count; i++)
            if (w->events.events[i].type == SIM_EVENT_CONTRACT_COMPLETE &&
                w->events.events[i].player_id < 0) m->contract_completions++;
    }
}
#endif
