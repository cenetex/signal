#include "gossip.h"

#include "../shared/sha256.h"
#include "../shared/station_util.h"
#include <string.h>

contract_summary_t contract_summary_make(const contract_t *ct) {
    contract_summary_t s = {0};
    if (!ct) return s;
    s.active = ct->active;
    s.action = (uint8_t)ct->action;
    s.station_index = ct->station_index;
    s.commodity = (uint8_t)ct->commodity;
    s.required_grade = ct->required_grade;
    s.proof_flags = ct->proof_flags;
    s.required_prefix_class = ct->required_prefix_class;
    s.required_recipe_id = ct->required_recipe_id;
    memcpy(s.required_parent, ct->required_parent, sizeof(s.required_parent));
    s.forbidden_origin_mask = ct->forbidden_origin_mask;
    s.quantity_needed = ct->quantity_needed;
    s.base_price = ct->base_price;
    s.age_at_copy = ct->age;
    return s;
}

static bool contract_summary_matches(const contract_summary_t *a,
                                     const contract_summary_t *b) {
    return a->action == b->action &&
           a->station_index == b->station_index &&
           a->commodity == b->commodity &&
           a->required_grade == b->required_grade &&
           a->proof_flags == b->proof_flags &&
           a->required_prefix_class == b->required_prefix_class &&
           a->required_recipe_id == b->required_recipe_id &&
           memcmp(a->required_parent, b->required_parent,
                  sizeof(a->required_parent)) == 0 &&
           a->forbidden_origin_mask == b->forbidden_origin_mask;
}

static void knowledge_contract_subject_hash(const contract_summary_t *s,
                                            uint8_t out[32]) {
    if (!s || !out) return;
    uint8_t key[49] = {
        (uint8_t)KNOW_CONTRACT,
        s->action,
        s->station_index,
        s->commodity,
        s->required_grade,
        s->proof_flags,
        s->required_prefix_class,
    };
    key[7] = (uint8_t)(s->required_recipe_id & 0xffu);
    key[8] = (uint8_t)((s->required_recipe_id >> 8) & 0xffu);
    memcpy(&key[9], s->required_parent, 32);
    for (int i = 0; i < 8; i++)
        key[41 + i] = (uint8_t)((s->forbidden_origin_mask >> (8 * i)) & 0xffu);
    sha256_bytes(key, sizeof(key), out);
}

bool knowledge_item_from_contract_summary(const contract_summary_t *s,
                                          knowledge_item_t *out) {
    if (!s || !out || !s->active) return false;
    memset(out, 0, sizeof(*out));
    out->kind = (uint8_t)KNOW_CONTRACT;
    out->confidence = 255;
    out->salience = 160;
    out->payload_kind = (uint8_t)KNOW_PAYLOAD_CONTRACT_SUMMARY;
    knowledge_contract_subject_hash(s, out->subject_hash);
    memcpy(out->payload, s, sizeof(*s));
    return true;
}

bool contract_summary_from_knowledge_item(const knowledge_item_t *item,
                                          contract_summary_t *out) {
    if (!item || !out) return false;
    if (item->kind != (uint8_t)KNOW_CONTRACT) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_CONTRACT_SUMMARY)
        return false;
    memcpy(out, item->payload, sizeof(*out));
    return out->active;
}

void knowledge_view_configure(knowledge_view_t *view, uint8_t capacity) {
    if (!view) return;
    if (capacity > KNOWLEDGE_VIEW_MAX_CAP)
        capacity = KNOWLEDGE_VIEW_MAX_CAP;
    view->capacity = capacity;
    uint8_t old_count = view->count <= KNOWLEDGE_VIEW_MAX_CAP
        ? view->count
        : KNOWLEDGE_VIEW_MAX_CAP;
    if (old_count > view->capacity) {
        int drop = (int)old_count - (int)view->capacity;
        for (int i = 0; i < view->capacity; i++)
            view->items[i] = view->items[i + drop];
        for (int i = view->capacity; i < KNOWLEDGE_VIEW_MAX_CAP; i++)
            memset(&view->items[i], 0, sizeof(view->items[i]));
        view->count = view->capacity;
    } else if (view->count > old_count) {
        view->count = old_count;
    }
}

static uint8_t knowledge_view_capacity(const knowledge_view_t *view) {
    if (!view) return 0;
    return view->capacity <= KNOWLEDGE_VIEW_MAX_CAP
        ? view->capacity
        : KNOWLEDGE_VIEW_MAX_CAP;
}

static bool knowledge_items_match(const knowledge_item_t *a,
                                  const knowledge_item_t *b) {
    return a->kind == b->kind &&
           a->payload_kind == b->payload_kind &&
           memcmp(a->subject_hash, b->subject_hash, 32) == 0;
}

static bool knowledge_item_is_newer_or_equal(const knowledge_item_t *incoming,
                                             const knowledge_item_t *stored) {
    contract_summary_t in_contract;
    contract_summary_t stored_contract;
    if (contract_summary_from_knowledge_item(incoming, &in_contract) &&
        contract_summary_from_knowledge_item(stored, &stored_contract)) {
        return in_contract.age_at_copy >= stored_contract.age_at_copy;
    }
    return incoming->learned_tick >= stored->learned_tick;
}

void knowledge_view_insert(knowledge_view_t *view, const knowledge_item_t *item) {
    if (!view || !item || item->kind == (uint8_t)KNOW_NONE) return;
    uint8_t cap = knowledge_view_capacity(view);
    if (cap == 0) return;

    if (view->count > cap) view->count = cap;
    for (int i = 0; i < view->count; i++) {
        if (knowledge_items_match(&view->items[i], item)) {
            if (knowledge_item_is_newer_or_equal(item, &view->items[i]))
                view->items[i] = *item;
            return;
        }
    }

    if (view->count < cap) {
        view->items[view->count++] = *item;
    } else {
        for (int i = 1; i < cap; i++)
            view->items[i - 1] = view->items[i];
        view->items[cap - 1] = *item;
    }
}

void knowledge_view_exchange(knowledge_view_t *a, knowledge_view_t *b) {
    if (!a || !b) return;
    uint8_t a_count = a->count;
    uint8_t b_count = b->count;
    if (a_count > knowledge_view_capacity(a)) a_count = knowledge_view_capacity(a);
    if (b_count > knowledge_view_capacity(b)) b_count = knowledge_view_capacity(b);

    knowledge_item_t a_pre[KNOWLEDGE_VIEW_MAX_CAP];
    knowledge_item_t b_pre[KNOWLEDGE_VIEW_MAX_CAP];
    for (int i = 0; i < a_count; i++) a_pre[i] = a->items[i];
    for (int i = 0; i < b_count; i++) b_pre[i] = b->items[i];

    for (int i = 0; i < a_count; i++) {
        if (a_pre[i].kind == (uint8_t)KNOW_NONE) continue;
        if (a_pre[i].hops < 255) a_pre[i].hops++;
        knowledge_view_insert(b, &a_pre[i]);
    }
    for (int i = 0; i < b_count; i++) {
        if (b_pre[i].kind == (uint8_t)KNOW_NONE) continue;
        if (b_pre[i].hops < 255) b_pre[i].hops++;
        knowledge_view_insert(a, &b_pre[i]);
    }
}

static void knowledge_seed_contract_pool(knowledge_view_t *view,
                                         const contract_summary_t *list,
                                         uint8_t count, int cap) {
    if (!view || !list || cap <= 0) return;
    int n = count < cap ? count : cap;
    for (int i = 0; i < n; i++) {
        if (!list[i].active) continue;
        knowledge_item_t item;
        if (knowledge_item_from_contract_summary(&list[i], &item))
            knowledge_view_insert(view, &item);
    }
}

void knowledge_view_forget_contract(knowledge_view_t *view, uint8_t action,
                                    int station_idx, commodity_t commodity) {
    if (!view || station_idx < 0 || station_idx > 255) return;
    int out = 0;
    uint8_t cap = knowledge_view_capacity(view);
    for (int i = 0; i < view->count && i < cap; i++) {
        const knowledge_item_t *item = &view->items[i];
        bool drop = false;
        contract_summary_t s;
        if (contract_summary_from_knowledge_item(item, &s)) {
            drop = s.action == action &&
                   s.station_index == (uint8_t)station_idx &&
                   s.commodity == (uint8_t)commodity;
        }
        if (!drop) {
            if (out != i) view->items[out] = view->items[i];
            out++;
        }
    }
    for (int i = out; i < view->count && i < cap; i++)
        memset(&view->items[i], 0, sizeof(view->items[i]));
    view->count = (uint8_t)out;
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
                           uint8_t *ship_count, int ship_cap,
                           knowledge_view_t *ship_knowledge) {
    if (!w || !ship_pool || !ship_count) return;
    if (station_index < 0 || station_index >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_index];
    knowledge_view_configure(&st->knowledge, STATION_KNOWN_ITEM_CAP);
    if (ship_knowledge)
        knowledge_view_configure(ship_knowledge, SHIP_KNOWN_ITEM_CAP);

    knowledge_seed_contract_pool(&st->knowledge, st->known_contracts,
                                 st->known_contract_count,
                                 STATION_KNOWN_CONTRACT_CAP);
    if (ship_knowledge) {
        knowledge_seed_contract_pool(ship_knowledge, ship_pool, *ship_count,
                                     ship_cap);
    }

    /* 1. Merge this station's locally-issued contracts into its own
     *    known pool. Tractor/fracture contracts are local by station_index.
     *    Delivery contracts are visible at both ends: station_index is the
     *    destination, target_index is the recourse origin. */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &w->contracts[k];
        if (!ct->active) continue;
        if (ct->station_index != station_index &&
            !(ct->action == CONTRACT_DELIVERY &&
              ct->target_index == station_index)) {
            continue;
        }
        contract_summary_t s = contract_summary_make(ct);
        contract_pool_insert(st->known_contracts, &st->known_contract_count,
                             STATION_KNOWN_CONTRACT_CAP, &s);
        knowledge_item_t item;
        if (knowledge_item_from_contract_summary(&s, &item))
            knowledge_view_insert(&st->knowledge, &item);
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

    if (ship_knowledge)
        knowledge_view_exchange(&st->knowledge, ship_knowledge);
}

void gossip_bootstrap_world_stations(world_t *w) {
    if (!w) return;
    for (int s_idx = 0; s_idx < MAX_STATIONS; s_idx++) {
        if (!station_is_active(&w->stations[s_idx])) continue;
        knowledge_view_configure(&w->stations[s_idx].knowledge,
                                 STATION_KNOWN_ITEM_CAP);
    }
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &w->contracts[k];
        if (!ct->active) continue;
        contract_summary_t s = contract_summary_make(ct);
        knowledge_item_t item;
        bool has_item = knowledge_item_from_contract_summary(&s, &item);
        for (int s_idx = 0; s_idx < MAX_STATIONS; s_idx++) {
            if (!station_is_active(&w->stations[s_idx])) continue;
            contract_pool_insert(w->stations[s_idx].known_contracts,
                                 &w->stations[s_idx].known_contract_count,
                                 STATION_KNOWN_CONTRACT_CAP, &s);
            if (has_item)
                knowledge_view_insert(&w->stations[s_idx].knowledge, &item);
        }
    }
}
