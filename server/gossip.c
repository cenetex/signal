#include "gossip.h"

#include <string.h>

contract_summary_t contract_summary_make(const contract_t *ct) {
    contract_summary_t s = {0};
    if (!ct) return s;
    s.active = ct->active;
    s.action = (uint8_t)ct->action;
    s.station_index = ct->station_index;
    s.commodity = (uint8_t)ct->commodity;
    s.required_grade = ct->required_grade;
    s.quantity_needed = ct->quantity_needed;
    s.base_price = ct->base_price;
    s.age_at_copy = ct->age;
    return s;
}

static bool contract_summary_matches(const contract_summary_t *a,
                                     const contract_summary_t *b) {
    return a->action == b->action &&
           a->station_index == b->station_index &&
           a->commodity == b->commodity;
}

void contract_pool_insert(contract_summary_t *list, uint8_t *count, int cap,
                          const contract_summary_t *s) {
    if (!list || !count || !s || cap <= 0) return;
    /* Dedup-on-match: keep whichever snapshot is "newer" (larger
     * age_at_copy = snapshotted later in the contract's life). This
     * stops an older snapshot from clobbering a fresher one when ships
     * arrive out of order. */
    for (int i = 0; i < *count; i++) {
        if (contract_summary_matches(&list[i], s)) {
            if (s->age_at_copy >= list[i].age_at_copy) {
                list[i] = *s;
            }
            return;
        }
    }
    if (*count < cap) {
        list[*count] = *s;
        (*count)++;
    } else {
        /* FIFO eviction: oldest entry (slot 0) drops, others shift left. */
        for (int i = 1; i < cap; i++)
            list[i-1] = list[i];
        list[cap - 1] = *s;
    }
}

void gossip_dock_handshake(world_t *w, int station_index,
                           contract_summary_t *ship_pool,
                           uint8_t *ship_count, int ship_cap) {
    if (!w || !ship_pool || !ship_count) return;
    if (station_index < 0 || station_index >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_index];

    /* 1. Merge this station's locally-issued contracts into its own
     *    known pool. Only filter for active and station_index == self,
     *    so we read this station's own state — local operation. */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &w->contracts[k];
        if (!ct->active) continue;
        if (ct->station_index != station_index) continue;
        contract_summary_t s = contract_summary_make(ct);
        contract_pool_insert(st->known_contracts, &st->known_contract_count,
                             STATION_KNOWN_CONTRACT_CAP, &s);
    }

    /* 2. Bidirectional copy. Snapshot ship's pre-handshake set so we
     *    don't double-pump after station-side merges back into ship. */
    contract_summary_t ship_pre[SHIP_KNOWN_CONTRACT_CAP];
    int max_ship_pre = ship_cap < SHIP_KNOWN_CONTRACT_CAP ? ship_cap : SHIP_KNOWN_CONTRACT_CAP;
    uint8_t ship_pre_count = *ship_count < max_ship_pre ? *ship_count : (uint8_t)max_ship_pre;
    for (int i = 0; i < ship_pre_count; i++)
        ship_pre[i] = ship_pool[i];

    /* Station → ship */
    for (int i = 0; i < st->known_contract_count; i++) {
        if (!st->known_contracts[i].active) continue;
        contract_pool_insert(ship_pool, ship_count, ship_cap,
                             &st->known_contracts[i]);
    }
    /* Ship → station */
    for (int i = 0; i < ship_pre_count; i++) {
        if (!ship_pre[i].active) continue;
        contract_pool_insert(st->known_contracts, &st->known_contract_count,
                             STATION_KNOWN_CONTRACT_CAP, &ship_pre[i]);
    }
}
