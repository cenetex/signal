#include "game_sim.h"
#include <string.h>

static bool any_bytes(const uint8_t *p, size_t n) {
    uint8_t value = 0;
    for (size_t i = 0; i < n; i++) value |= p[i];
    return value != 0;
}

bool world_fly_purchases_valid(const world_t *w) {
    if (!w || w->fly_purchase_count > MAX_FLY_PURCHASES) return false;
    for (uint32_t i = 0; i < w->fly_purchase_count; i++) {
        const fly_purchase_t *p = &w->fly_purchases[i];
        const ship_asset_t *asset = world_ship_asset_by_id_const(w, p->asset_id);
        if (p->station > 2 || !any_bytes(p->purchase_id, 32) ||
            !any_bytes(p->wallet, 32) ||
            !asset || asset->provenance != SHIP_ASSET_PROVENANCE_FLY_PURCHASE ||
            asset->owner_principal.kind != ACTOR_PRINCIPAL_PLAYER ||
            memcmp(asset->owner_principal.id, p->wallet, 32)) return false;
        if (!any_bytes(p->burn_signature, 64) &&
            (asset->destroyed || asset->status != SHIP_ASSET_STATUS_STORED ||
             asset->operator_kind != SHIP_ASSET_OPERATOR_NONE)) return false;
        for (uint32_t j = 0; j < i; j++) {
            const fly_purchase_t *prior = &w->fly_purchases[j];
            if (!memcmp(prior->purchase_id, p->purchase_id, 32) ||
                (any_bytes(p->burn_signature, 64) && !memcmp(prior->burn_signature, p->burn_signature, 64)) ||
                prior->asset_id == p->asset_id) return false;
        }
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active || asset->provenance != SHIP_ASSET_PROVENANCE_FLY_PURCHASE) continue;
        bool found = false;
        for (uint32_t j = 0; j < w->fly_purchase_count; j++)
            if (w->fly_purchases[j].asset_id == asset->asset_id) found = true;
        if (!found) return false;
    }
    return true;
}

static const fly_purchase_t *fly_purchase_apply(world_t *w,
    const uint8_t id[32], const uint8_t wallet[32], const uint8_t signature[64], int station) {
    if (!w || !id || !wallet || !signature || station < 0 || station > 2 ||
        !any_bytes(id, 32) ||
        !any_bytes(wallet, 32) ||
        w->fly_purchase_count > MAX_FLY_PURCHASES) return NULL;
    for (uint32_t i = 0; i < w->fly_purchase_count; i++) {
        fly_purchase_t *p = &w->fly_purchases[i];
        if (!memcmp(p->purchase_id, id, 32)) {
            if (p->station != station || memcmp(p->wallet, wallet, 32)) return NULL;
            if (any_bytes(signature, 64)) {
                if (any_bytes(p->burn_signature, 64) && memcmp(p->burn_signature, signature, 64)) return NULL;
                for (uint32_t j = 0; j < w->fly_purchase_count; j++)
                    if (j != i && !memcmp(w->fly_purchases[j].burn_signature, signature, 64)) return NULL;
                memcpy(p->burn_signature, signature, 64);
            }
            ship_asset_t *asset = world_ship_asset_by_id(w, p->asset_id);
            if (asset) (void)ship_asset_launch_fly_worker(w, asset, station);
            return p;
        }
        if (any_bytes(signature, 64) && !memcmp(p->burn_signature, signature, 64)) return NULL;
    }
    if (w->fly_purchase_count == MAX_FLY_PURCHASES ||
        !station_is_active(&w->stations[station])) return NULL;
    actor_principal_t owner = actor_principal_none();
    if (!actor_principal_from_stable_id(ACTOR_PRINCIPAL_PLAYER, wallet, &owner)) return NULL;
    hull_class_t hull = station == 1 ? HULL_CLASS_DRONE_TRACTOR : HULL_CLASS_NPC_MINER;
    ship_asset_t *asset = world_ship_asset_mint(w, hull, &owner, station,
        SHIP_ASSET_PROVENANCE_FLY_PURCHASE, false, station);
    if (!asset) return NULL;
    if (station == 2) asset->stored_ship.mining_level = 2;
    fly_purchase_t *p = &w->fly_purchases[w->fly_purchase_count++];
    memset(p, 0, sizeof(*p));
    memcpy(p->purchase_id, id, 32);
    memcpy(p->wallet, wallet, 32);
    memcpy(p->burn_signature, signature, 64);
    p->asset_id = asset->asset_id;
    p->station = (uint8_t)station;
    (void)ship_asset_launch_fly_worker(w, asset, station);
    return p;
}

const fly_purchase_t *world_fly_purchase_reserve(world_t *w,
    const uint8_t id[32], const uint8_t wallet[32], int station) {
    const uint8_t unsigned_receipt[64] = {0};
    return fly_purchase_apply(w, id, wallet, unsigned_receipt, station);
}

const fly_purchase_t *world_fly_purchase_grant(world_t *w,
    const uint8_t id[32], const uint8_t wallet[32], const uint8_t signature[64], int station) {
    if (!signature || !any_bytes(signature, 64)) return NULL;
    return fly_purchase_apply(w, id, wallet, signature, station);
}

/* Read the active worker's own station ledgers without creating accounts. */
bool world_fly_worker_credits(const world_t *w, uint32_t asset_id, double *out) {
    if (!w || !out) return false;
    const ship_asset_t *asset = world_ship_asset_by_id_const(w, asset_id);
    if (!asset || asset->provenance != SHIP_ASSET_PROVENANCE_FLY_PURCHASE ||
        asset->operator_kind != SHIP_ASSET_OPERATOR_NPC ||
        asset->operator_slot < 0 || asset->operator_slot >= MAX_NPC_SHIPS)
        return false;
    const npc_ship_t *npc = &w->npc_ships[asset->operator_slot];
    if (!npc->active || npc->ship_asset_id != asset_id) return false;
    double total = 0.0;
    for (int i = 0; i < MAX_STATIONS; i++)
        total += ledger_balance(&w->stations[i], npc->session_token);
    *out = total;
    return true;
}
