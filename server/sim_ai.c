/*
 * sim_ai.c -- NPC ship subsystem.
 * Extracted from game_sim.c: target finding, steering, physics,
 * spawn, state machines (MINER / HAULER / TOW), and the per-tick
 * step_npc_ships() dispatcher.
 */
#include "sim_ai.h"
#include "tractor.h"
#include "sim_nav.h"
#include "sim_flight.h"
#include "signal_brain.h"
#include "sim_ship.h"
#include "sim_physics.h"
#include "sim_mining.h"
#include "sim_construction.h"
#include "signal_model.h"
#include "manifest.h"
#include "commodity.h"
#include "contract_fit.h"
#include "ship.h"
#include "game_sim.h" /* SHIP_COLLISION_DAMAGE_THRESHOLD/_SCALE */
#include "cargo_receipt_issue.h"
#include "gossip.h"
#include "chain_log.h"
#include "sha256.h"
#include "station_authority.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Remove up to `n` cargo units of `c` from a station's manifest.
 * Returns the number actually removed. Walks backward so removing
 * doesn't disturb earlier indices. Used by NPC haulers so the
 * manifest stays in lockstep with the inventory float; otherwise the
 * trade picker (manifest-only) shows phantom rows for stock the
 * hauler already carried away. */
static int station_manifest_drain_commodity(station_t *st, commodity_t c, int n) {
    return station_manifest_consume_by_commodity(st, c, n);
}

#define FRONTIER_DIRECTOR_BASE_INTERVAL 30.0f
#define FRONTIER_DIRECTOR_MIN_INTERVAL 1.0f
#define FRONTIER_DIRECTOR_MAX_PLANNED 8

typedef struct {
    module_type_t type;
    uint8_t ring;
    uint8_t slot;
} frontier_starter_plan_t;

static const frontier_starter_plan_t FRONTIER_STARTER_PLANS[] = {
    { MODULE_HOPPER,  1, 1 },
    { MODULE_FURNACE, 2, 2 },
};

static int frontier_planned_limit(int virtual_pilots) {
    if (virtual_pilots <= 0) return 0;
    int limit = 1 + virtual_pilots / 250;
    if (limit > FRONTIER_DIRECTOR_MAX_PLANNED)
        limit = FRONTIER_DIRECTOR_MAX_PLANNED;
    return limit;
}

static float frontier_director_interval(int virtual_pilots) {
    if (virtual_pilots <= 0) return FRONTIER_DIRECTOR_BASE_INTERVAL;
    float n = sqrtf((float)virtual_pilots);
    if (n < 1.0f) n = 1.0f;
    float interval = FRONTIER_DIRECTOR_BASE_INTERVAL / n;
    if (interval < FRONTIER_DIRECTOR_MIN_INTERVAL)
        interval = FRONTIER_DIRECTOR_MIN_INTERVAL;
    return interval;
}

static int frontier_virtual_logistics_budget(int virtual_pilots) {
    if (virtual_pilots <= 0) return 0;
    int budget = 1 + virtual_pilots / 250;
    if (budget > 8) budget = 8;
    return budget;
}

void frontier_virtual_pilots_set(world_t *w, int count) {
    if (!w) return;
    if (count < 0) count = 0;
    if (count > SIGNAL_FRONTIER_VIRTUAL_PILOTS_MAX)
        count = SIGNAL_FRONTIER_VIRTUAL_PILOTS_MAX;
    w->frontier_virtual_pilots = count;
    if (count > 0 && w->frontier_plan_timer <= 0.0f)
        w->frontier_plan_timer = 0.1f;
}

static int frontier_count_planned_outposts(const world_t *w) {
    int count = 0;
    if (!w) return 0;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (w->stations[s].planned) count++;
    }
    return count;
}

static int frontier_count_scaffold_work(const world_t *w, module_type_t type) {
    int count = 0;
    if (!w) return 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int i = 0; i < st->pending_scaffold_count; i++) {
            if (st->pending_scaffolds[i].type == type)
                count++;
        }
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active || sc->module_type != type) continue;
        if (sc->state == SCAFFOLD_NASCENT ||
            sc->state == SCAFFOLD_LOOSE ||
            sc->state == SCAFFOLD_TOWING ||
            sc->state == SCAFFOLD_SNAPPING)
            count++;
    }
    return count;
}

static int frontier_count_module_plans(const world_t *w, module_type_t type) {
    int count = 0;
    if (!w) return 0;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        if (st->planned_owner != -1) continue;
        for (int p = 0; p < st->placement_plan_count; p++)
            if (st->placement_plans[p].type == type) count++;
    }
    return count;
}

static int frontier_find_free_station_slot(const world_t *w) {
    if (!w) return -1;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) return s;
    }
    return -1;
}

static void frontier_hash_u32(sha256_ctx_t *ctx, uint32_t v) {
    uint8_t le[4] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
    };
    sha256_update(ctx, le, sizeof(le));
}

static void frontier_founder_pubkey(const world_t *w, int slot, vec2 pos,
                                    uint8_t out[32]) {
    static const uint8_t domain[] = {
        'S','I','G','N','A','L','-','F','R','O','N','T','I','E','R',
        '-','P','I','L','O','T','-','v','1'
    };
    sha256_ctx_t ctx;
    uint32_t x_bits = 0;
    uint32_t y_bits = 0;
    memcpy(&x_bits, &pos.x, sizeof(x_bits));
    memcpy(&y_bits, &pos.y, sizeof(y_bits));
    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain));
    frontier_hash_u32(&ctx, w ? w->belt_seed : 0);
    frontier_hash_u32(&ctx, (uint32_t)slot);
    frontier_hash_u32(&ctx, w ? w->frontier_plans_created : 0);
    frontier_hash_u32(&ctx, x_bits);
    frontier_hash_u32(&ctx, y_bits);
    sha256_final(&ctx, out);
}

static bool frontier_queue_scaffold_order(world_t *w, module_type_t type) {
    if (!w) return false;
    commodity_t mat = module_build_material_lookup(type);
    int best_station = -1;
    int best_pending = 999;
    float best_stock = -1.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_sells_scaffold(st, type)) continue;
        if (st->pending_scaffold_count >= 4) continue;
        float stock = (mat < COMMODITY_COUNT) ? st->_inventory_cache[mat] : 0.0f;
        if (best_station < 0 ||
            st->pending_scaffold_count < best_pending ||
            (st->pending_scaffold_count == best_pending && stock > best_stock)) {
            best_station = s;
            best_pending = st->pending_scaffold_count;
            best_stock = stock;
        }
    }
    if (best_station < 0) return false;
    station_t *st = &w->stations[best_station];
    int idx = st->pending_scaffold_count++;
    st->pending_scaffolds[idx].type = type;
    st->pending_scaffolds[idx].owner = -1;
    if (type == MODULE_SIGNAL_RELAY)
        w->frontier_scaffold_orders++;
    else
        w->frontier_module_scaffold_orders++;
    SIM_LOG("[frontier] queued %s scaffold at station %d\n",
            module_type_name(type), best_station);
    return true;
}

static void frontier_remove_pending_scaffold(station_t *st, int idx) {
    if (!st || idx < 0 || idx >= st->pending_scaffold_count) return;
    for (int i = idx; i < st->pending_scaffold_count - 1; i++)
        st->pending_scaffolds[i] = st->pending_scaffolds[i + 1];
    st->pending_scaffold_count--;
}

static bool frontier_virtual_manufacture_type(world_t *w, module_type_t type) {
    if (!w) return false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int p = 0; p < st->pending_scaffold_count; p++) {
            if (st->pending_scaffolds[p].type != type) continue;
            int owner = st->pending_scaffolds[p].owner;
            int idx = spawn_scaffold(w, type, st->pos, owner);
            if (idx < 0) return false;
            scaffold_t *sc = &w->scaffolds[idx];
            sc->state = SCAFFOLD_LOOSE;
            sc->owner = owner;
            sc->built_at_station = -1;
            float angle = (float)((w->frontier_virtual_scaffolds_manufactured % 16u)
                                  * 0.3926990817f);
            sc->pos = v2_add(st->pos, v2(cosf(angle) * 180.0f,
                                         sinf(angle) * 180.0f));
            sc->vel = v2(0.0f, 0.0f);
            frontier_remove_pending_scaffold(st, p);
            w->frontier_virtual_scaffolds_manufactured++;
            SIM_LOG("[frontier] virtual pilots manufactured %s scaffold at station %d\n",
                    module_type_name(type), s);
            return true;
        }
    }
    return false;
}

static bool frontier_virtual_manufacture_one(world_t *w) {
    static const module_type_t PRIORITY[] = {
        MODULE_SIGNAL_RELAY,
        MODULE_HOPPER,
        MODULE_FURNACE,
    };
    for (int i = 0; i < (int)(sizeof(PRIORITY) / sizeof(PRIORITY[0])); i++)
        if (frontier_virtual_manufacture_type(w, PRIORITY[i])) return true;
    return false;
}

static int frontier_find_planned_relay_destination(const world_t *w) {
    if (!w) return -1;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (st->planned) return s;
    }
    return -1;
}

static bool frontier_slot_occupied_or_inflight(const world_t *w,
                                               int station_idx,
                                               int ring,
                                               int slot) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return true;
    const station_t *st = &w->stations[station_idx];
    for (int m = 0; m < st->module_count; m++) {
        if ((int)st->modules[m].ring != ring) continue;
        if ((int)st->modules[m].slot != slot) continue;
        return true;
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        if (sc->state != SCAFFOLD_SNAPPING) continue;
        if (sc->placed_station != station_idx) continue;
        if (sc->placed_ring != ring || sc->placed_slot != slot) continue;
        return true;
    }
    return false;
}

static int frontier_find_active_module_destination(const world_t *w,
                                                  module_type_t type,
                                                  int *out_ring,
                                                  int *out_slot) {
    if (!w) return -1;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            if (st->placement_plans[p].type != type) continue;
            int ring = st->placement_plans[p].ring;
            int slot = st->placement_plans[p].slot;
            if (frontier_slot_occupied_or_inflight(w, s, ring, slot))
                continue;
            if (out_ring) *out_ring = ring;
            if (out_slot) *out_slot = slot;
            return s;
        }
    }
    return -1;
}

static bool frontier_virtual_deliver_one(world_t *w) {
    if (!w) return false;
    static const module_type_t PRIORITY[] = {
        MODULE_SIGNAL_RELAY,
        MODULE_HOPPER,
        MODULE_FURNACE,
    };
    for (int pi = 0; pi < (int)(sizeof(PRIORITY) / sizeof(PRIORITY[0])); pi++) {
        module_type_t type = PRIORITY[pi];
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            scaffold_t *sc = &w->scaffolds[i];
            if (!sc->active) continue;
            if (sc->state != SCAFFOLD_LOOSE) continue;
            if (sc->owner >= 0 || sc->towed_by >= 0) continue;
            if (sc->module_type != type) continue;
            if (type == MODULE_SIGNAL_RELAY && sc->placed_station >= 0)
                continue;

            if (type == MODULE_SIGNAL_RELAY) {
                int dest = frontier_find_planned_relay_destination(w);
                if (dest < 0) continue;
                station_t *st = &w->stations[dest];
                sc->pos = st->pos;
                sc->vel = v2(0.0f, 0.0f);
                sc->placed_station = dest;
                w->frontier_virtual_scaffold_deliveries++;
                SIM_LOG("[frontier] virtual pilots delivered signal relay scaffold to station %d\n",
                        dest);
                return true;
            }

            int ring = -1, slot = -1;
            int dest = frontier_find_active_module_destination(w, type,
                                                               &ring, &slot);
            if (dest < 0) continue;
            station_t *st = &w->stations[dest];
            sc->state = SCAFFOLD_SNAPPING;
            sc->placed_station = dest;
            sc->placed_ring = ring;
            sc->placed_slot = slot;
            sc->pos = module_world_pos_ring(st, ring, slot);
            sc->vel = v2(0.0f, 0.0f);
            w->frontier_virtual_scaffold_deliveries++;
            SIM_LOG("[frontier] virtual pilots delivered %s scaffold to station %d ring %d slot %d\n",
                    module_type_name(type), dest, ring, slot);
            return true;
        }
    }
    return false;
}

static bool frontier_virtual_supply_one(world_t *w) {
    if (!w) return false;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st) || st->planned) continue;
        if (st->scaffold && st->scaffold_progress < 1.0f) {
            st->scaffold_progress = 1.0f;
            activate_outpost(w, s);
            w->frontier_virtual_supply_deliveries++;
            SIM_LOG("[frontier] virtual pilots supplied station scaffold at station %d\n", s);
            return true;
        }
    }
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st) || st->scaffold) continue;
        for (int m = 0; m < st->module_count; m++) {
            station_module_t *mod = &st->modules[m];
            if (!mod->scaffold) continue;
            if (mod->build_progress >= 1.0f) continue;
            mod->build_progress = 1.0f;
            w->frontier_virtual_supply_deliveries++;
            SIM_LOG("[frontier] virtual pilots supplied %s at station %d\n",
                    module_type_name(mod->type), s);
            return true;
        }
    }
    return false;
}

static void frontier_run_virtual_logistics(world_t *w) {
    int budget = frontier_virtual_logistics_budget(w ? w->frontier_virtual_pilots : 0);
    for (int i = 0; i < budget; i++) {
        if (frontier_virtual_supply_one(w)) continue;
        if (frontier_virtual_deliver_one(w)) continue;
        if (frontier_virtual_manufacture_one(w)) continue;
        break;
    }
}

static bool frontier_plan_outpost(world_t *w) {
    if (!w) return false;
    int slot = frontier_find_free_station_slot(w);
    if (slot < 0) return false;

    const float golden_angle = 2.39996323f;
    uint32_t seed = w->frontier_plans_created * 17u + w->tick * 3u + 11u;
    for (int pass = 0; pass < MAX_STATIONS; pass++) {
        int parent_idx = (int)((seed + (uint32_t)pass) % (uint32_t)MAX_STATIONS);
        const station_t *parent = &w->stations[parent_idx];
        if (!station_is_active(parent)) continue;
        if (!station_provides_signal(parent)) continue;
        if (parent->signal_range <= OUTPOST_MIN_DISTANCE * 2.0f) continue;

        for (int a = 0; a < 24; a++) {
            uint32_t k = seed + (uint32_t)pass * 24u + (uint32_t)a;
            float angle = (float)k * golden_angle;
            float band = 0.55f + 0.30f * (float)(k % 7u) / 6.0f;
            float dist = parent->signal_range * band;
            if (dist < OUTPOST_MIN_DISTANCE * 1.25f)
                dist = OUTPOST_MIN_DISTANCE * 1.25f;
            vec2 pos = v2_add(parent->pos,
                              v2(cosf(angle) * dist, sinf(angle) * dist));
            if (!can_place_outpost(w, pos)) continue;

            if (slot >= w->station_count) w->station_count = slot + 1;
            station_t *st = &w->stations[slot];
            station_cleanup(st);
            memset(st, 0, sizeof(*st));
            (void)station_manifest_bootstrap(st);
            if (w->next_station_id == 0) w->next_station_id = 1;
            st->id = w->next_station_id++;
            snprintf(st->name, sizeof(st->name), "Frontier %02d", slot);
            st->pos = pos;
            st->planned = true;
            st->planned_owner = -1;

            uint8_t founder[32];
            frontier_founder_pubkey(w, slot, pos, founder);
            station_authority_init_outpost(st, founder,
                                           (uint64_t)(w->time * 128.0f));
            chain_log_health_set(st, CHAIN_HEALTH_FRESH, false, 0, NULL,
                                 "virtual frontier pilot planned outpost");
            st->radius = 0.0f;
            st->dock_radius = 0.0f;
            st->signal_range = 0.0f;
            st->arm_count = 0;
            for (int r = 0; r < MAX_ARMS; r++) {
                st->arm_rotation[r] = 0.0f;
                st->ring_offset[r] = 0.0f;
                st->arm_speed[r] = 0.0f;
            }

            w->frontier_plans_created++;
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_OUTPOST_PLACED,
                .player_id = -1,
                .outpost_placed = { .slot = slot },
            });
            SIM_LOG("[frontier] virtual pilots planned outpost at slot %d\n", slot);
            return true;
        }
    }
    return false;
}

static bool frontier_station_has_module_or_plan(const station_t *st,
                                                module_type_t type) {
    if (!st) return false;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == type) return true;
    }
    for (int p = 0; p < st->placement_plan_count; p++) {
        if (st->placement_plans[p].type == type) return true;
    }
    return false;
}

static bool frontier_slot_reserved(const station_t *st, int ring, int slot) {
    if (!st) return true;
    for (int m = 0; m < st->module_count; m++) {
        if ((int)st->modules[m].ring == ring &&
            (int)st->modules[m].slot == slot) {
            return true;
        }
    }
    for (int p = 0; p < st->placement_plan_count; p++) {
        if ((int)st->placement_plans[p].ring == ring &&
            (int)st->placement_plans[p].slot == slot) {
            return true;
        }
    }
    return false;
}

static bool frontier_add_module_plan(world_t *w, int station_idx,
                                     const frontier_starter_plan_t *plan) {
    if (!w || !plan) return false;
    if (station_idx < SIGNAL_FIRST_OUTPOST_INDEX || station_idx >= MAX_STATIONS) return false;
    station_t *st = &w->stations[station_idx];
    if (!station_exists(st)) return false;
    if (st->planned_owner != -1) return false;
    if (st->placement_plan_count >= 8) return false;
    if (frontier_station_has_module_or_plan(st, plan->type)) return false;
    if (frontier_slot_reserved(st, plan->ring, plan->slot)) return false;
    int idx = st->placement_plan_count++;
    st->placement_plans[idx].type = plan->type;
    st->placement_plans[idx].ring = plan->ring;
    st->placement_plans[idx].slot = plan->slot;
    st->placement_plans[idx].owner = -1;
    w->frontier_module_plans_created++;
    SIM_LOG("[frontier] planned %s at station %d ring %u slot %u\n",
            module_type_name(plan->type), station_idx,
            (unsigned)plan->ring, (unsigned)plan->slot);
    return true;
}

static int frontier_ensure_starter_module_plans(world_t *w) {
    int created = 0;
    if (!w) return 0;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        if (!st->planned && !station_is_active(st)) continue;
        if (st->planned_owner != -1) continue;
        for (int i = 0; i < (int)(sizeof(FRONTIER_STARTER_PLANS) /
                                  sizeof(FRONTIER_STARTER_PLANS[0])); i++) {
            if (frontier_add_module_plan(w, s, &FRONTIER_STARTER_PLANS[i]))
                created++;
        }
    }
    return created;
}

static void frontier_queue_scaffolds_for_module_plans(world_t *w) {
    if (!w) return;
    for (int i = 0; i < (int)(sizeof(FRONTIER_STARTER_PLANS) /
                              sizeof(FRONTIER_STARTER_PLANS[0])); i++) {
        module_type_t type = FRONTIER_STARTER_PLANS[i].type;
        int needed = frontier_count_module_plans(w, type);
        int work = frontier_count_scaffold_work(w, type);
        while (work < needed) {
            if (!frontier_queue_scaffold_order(w, type)) break;
            work++;
        }
    }
}

void step_frontier_director(world_t *w, float dt) {
    if (!w || w->frontier_virtual_pilots <= 0 || dt <= 0.0f) return;
    w->frontier_plan_timer -= dt;
    if (w->frontier_plan_timer > 0.0f) return;

    frontier_run_virtual_logistics(w);

    int planned = frontier_count_planned_outposts(w);
    int plan_limit = frontier_planned_limit(w->frontier_virtual_pilots);
    if (planned < plan_limit && frontier_plan_outpost(w))
        planned++;

    int relay_work = frontier_count_scaffold_work(w, MODULE_SIGNAL_RELAY);
    if (relay_work < planned)
        (void)frontier_queue_scaffold_order(w, MODULE_SIGNAL_RELAY);

    (void)frontier_ensure_starter_module_plans(w);
    frontier_queue_scaffolds_for_module_plans(w);

    w->frontier_plan_timer = frontier_director_interval(w->frontier_virtual_pilots);
}

static int hauler_reserve_units(void) {
    return (int)ceilf(HAULER_RESERVE - 0.0001f);
}

static bool npc_hash32_is_zero(const uint8_t hash[32]) {
    static const uint8_t zero[32] = {0};
    return !hash || memcmp(hash, zero, sizeof(zero)) == 0;
}

static void npc_custody_pubkey(const npc_ship_t *npc, int npc_slot,
                               uint8_t out[32]) {
    static const uint8_t domain[] = {
        'S','I','G','N','A','L','-','N','P','C','-','C','U','S','T','O','D','Y','-','v','1'
    };
    sha256_ctx_t ctx;
    uint8_t slot_le[2];
    uint8_t role = npc ? (uint8_t)npc->role : 0;
    uint8_t home = npc ? (uint8_t)npc->home_station : 0xFFu;

    slot_le[0] = (uint8_t)(npc_slot & 0xFF);
    slot_le[1] = (uint8_t)((npc_slot >> 8) & 0xFF);

    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain));
    if (npc) sha256_update(&ctx, npc->session_token, sizeof(npc->session_token));
    sha256_update(&ctx, slot_le, sizeof(slot_le));
    sha256_update(&ctx, &role, sizeof(role));
    sha256_update(&ctx, &home, sizeof(home));
    sha256_final(&ctx, out);
}

static bool append_station_transfer_receipt(world_t *w, station_t *author,
                                            const uint8_t from_pubkey[32],
                                            const uint8_t to_pubkey[32],
                                            const cargo_unit_t *unit,
                                            cargo_receipt_chain_t *chain) {
    if (!w || !author || !unit || !chain) return false;
    if (npc_hash32_is_zero(unit->pub)) return false;
    if (chain->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) return false;

    uint8_t prev_hash[32] = {0};
    const uint8_t *prev = author->chain_last_hash;
    if (chain->len > 0) {
        cargo_receipt_hash(&chain->links[chain->len - 1], prev_hash);
        prev = prev_hash;
    }

    cargo_receipt_t receipt = {0};
    uint64_t xfer_id = cargo_receipt_emit_transfer(w, author,
                                                   from_pubkey, to_pubkey,
                                                   unit->pub, unit->kind,
                                                   prev, &receipt);
    if (xfer_id == 0) return false;
    chain->links[chain->len++] = receipt;
    return true;
}

static bool station_accepts_hauler_commodity(const station_t *st,
                                             commodity_t c) {
    if (!st || !station_is_active(st)) return false;
    if (c < COMMODITY_RAW_ORE_COUNT || c >= COMMODITY_COUNT) return false;

    if (st->scaffold && c == COMMODITY_FRAME) return true;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *m = &st->modules[i];
        if (module_build_state(m) != MODULE_BUILD_AWAITING_SUPPLY) continue;
        if (module_build_material_lookup(m->type) == c) return true;
    }
    return station_consumes(st, c);
}

static float station_finished_fraction_for_hauler(const station_t *st,
                                                  commodity_t c) {
    if (!st || c < COMMODITY_RAW_ORE_COUNT || c >= COMMODITY_COUNT)
        return 0.0f;
    float v = st->_inventory_cache[c];
    float whole = floorf(v + 0.0001f);
    float frac = v - whole;
    return (frac > 0.0f && frac < 1.0f) ? frac : 0.0f;
}

static int station_finished_room_units_for_hauler(const station_t *st,
                                                  commodity_t c, float cap) {
    int stock = station_finished_count(st, c);
    float used = (float)stock + station_finished_fraction_for_hauler(st, c);
    int room = (int)floorf(cap - used + 0.0001f);
    return room > 0 ? room : 0;
}

static void contract_summary_pool_forget(contract_summary_t *list,
                                         uint8_t *count, int cap,
                                         int station_idx, commodity_t c) {
    if (!list || !count || cap <= 0) return;
    int out = 0;
    for (int i = 0; i < *count && i < cap; i++) {
        const contract_summary_t *cs = &list[i];
        bool drop = cs->active &&
                    cs->action == (uint8_t)CONTRACT_TRACTOR &&
                    cs->station_index == station_idx &&
                    cs->commodity == (uint8_t)c;
        if (!drop) {
            if (out != i) list[out] = list[i];
            out++;
        }
    }
    for (int i = out; i < *count && i < cap; i++)
        memset(&list[i], 0, sizeof(list[i]));
    *count = (uint8_t)out;
}

static void forget_known_delivery_contracts(world_t *w, int station_idx,
                                            commodity_t c) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        contract_summary_pool_forget(w->stations[s].known_contracts,
                                     &w->stations[s].known_contract_count,
                                     STATION_KNOWN_CONTRACT_CAP,
                                     station_idx, c);
        knowledge_view_forget_contract(&w->stations[s].knowledge,
                                       (uint8_t)CONTRACT_TRACTOR,
                                       station_idx, c);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        contract_summary_pool_forget(w->npc_ships[n].known_contracts,
                                     &w->npc_ships[n].known_contract_count,
                                     SHIP_KNOWN_CONTRACT_CAP,
                                     station_idx, c);
        knowledge_view_forget_contract(&w->npc_ships[n].knowledge,
                                       (uint8_t)CONTRACT_TRACTOR,
                                       station_idx, c);
    }
    for (int p = 0; p < MAX_PLAYERS; p++) {
        contract_summary_pool_forget(w->players[p].ship.known_contracts,
                                     &w->players[p].ship.known_contract_count,
                                     SHIP_KNOWN_CONTRACT_CAP,
                                     station_idx, c);
        knowledge_view_forget_contract(&w->players[p].ship.knowledge,
                                       (uint8_t)CONTRACT_TRACTOR,
                                       station_idx, c);
    }
}

static void expire_incompatible_delivery_contracts(world_t *w, int station_idx,
                                                   commodity_t c) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active) continue;
        if (ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index != station_idx) continue;
        if (ct->commodity != c) continue;
        ct->active = false;
    }
    forget_known_delivery_contracts(w, station_idx, c);
}

static contract_t *hauler_delivery_contract(world_t *w, int station_idx,
                                            commodity_t c,
                                            const manifest_t *manifest) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return NULL;
    station_t *dest = &w->stations[station_idx];
    if (!station_accepts_hauler_commodity(dest, c)) {
        expire_incompatible_delivery_contracts(w, station_idx, c);
        return NULL;
    }

    contract_t *best = NULL;
    float best_price = 0.0f;
    bool saw_active_contract = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active) continue;
        if (ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index != station_idx) continue;
        if (ct->commodity != c) continue;
        if (ct->quantity_needed <= 0.01f) continue;
        saw_active_contract = true;
        if (manifest) {
            if (contract_fit_manifest_count(ct, manifest) <= 0) continue;
        } else if (ct->required_grade > (uint8_t)MINING_GRADE_COMMON ||
                   ct->proof_flags != 0) {
            continue;
        }
        float price = contract_price(ct);
        if (price > best_price) {
            best_price = price;
            best = ct;
        }
    }
    if (!saw_active_contract)
        forget_known_delivery_contracts(w, station_idx, c);
    return best;
}

static bool hauler_contract_matches_summary(const contract_t *ct,
                                            const contract_summary_t *cs) {
    if (!ct || !cs) return false;
    if (!ct->active || !cs->active) return false;
    if ((uint8_t)ct->action != cs->action) return false;
    if (ct->station_index != cs->station_index) return false;
    if ((uint8_t)ct->commodity != cs->commodity) return false;
    if (ct->required_grade != cs->required_grade) return false;
    if (ct->proof_flags != cs->proof_flags) return false;
    if (ct->required_prefix_class != cs->required_prefix_class) return false;
    if (ct->required_recipe_id != cs->required_recipe_id) return false;
    if (ct->forbidden_origin_mask != cs->forbidden_origin_mask) return false;
    if (memcmp(ct->target_pub, cs->target_pub, sizeof(ct->target_pub)) != 0)
        return false;
    return memcmp(ct->required_parent, cs->required_parent,
                  sizeof(ct->required_parent)) == 0;
}

static contract_t *hauler_pickup_contract_from_summary(
    world_t *w, const contract_summary_t *cs, const manifest_t *manifest) {
    if (!w || !cs || !manifest || !cs->active) return NULL;
    if (cs->action != (uint8_t)CONTRACT_TRACTOR) return NULL;
    if (cs->station_index >= MAX_STATIONS) return NULL;
    if (cs->commodity < COMMODITY_RAW_ORE_COUNT ||
        cs->commodity >= COMMODITY_COUNT) return NULL;

    contract_t *best = NULL;
    float best_price = 0.0f;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!hauler_contract_matches_summary(ct, cs)) continue;
        if (ct->quantity_needed <= 0.01f) continue;
        if (contract_fit_manifest_count(ct, manifest) <= 0) continue;
        float price = contract_price(ct);
        if (price > best_price) {
            best_price = price;
            best = ct;
        }
    }
    return best;
}

static void npc_update_manifest_rarity_tint(npc_ship_t *npc,
                                            const ship_t *paired_ship,
                                            float dt) {
    float neutral_r = 0.86f, neutral_g = 0.93f, neutral_b = 1.0f;
    float target_r = neutral_r, target_g = neutral_g, target_b = neutral_b;

    if (paired_ship) {
        float cap = ship_cargo_capacity(paired_ship);
        float total = ship_total_cargo(paired_ship);
        float fill = (cap > 0.0f) ? (total / cap) : 0.0f;
        (void)manifest_rarity_tint(&paired_ship->manifest, fill,
                                   neutral_r, neutral_g, neutral_b,
                                   &target_r, &target_g, &target_b);
    }

    float blend = clampf(0.3f * dt, 0.0f, 1.0f);
    npc->tint_r = lerpf(npc->tint_r, target_r, blend);
    npc->tint_g = lerpf(npc->tint_g, target_g, blend);
    npc->tint_b = lerpf(npc->tint_b, target_b, blend);
}

/* Legacy fallback: push synthetic legacy-migrate units when an NPC has
 * no paired ship manifest. Normal hauler transit now moves real units
 * through npc_ship_for(...)->manifest and should not call this path. */
static int station_manifest_seed_from_npc(station_t *st, commodity_t c, int n,
                                          int npc_slot) {
    if (!st || n <= 0) return 0;
    if (st->manifest.cap == 0 && !station_manifest_bootstrap(st)) return 0;
    uint8_t origin[8] = { 'N','P','C','D','0','0','0','0' };
    origin[7] = (uint8_t)('0' + (npc_slot % 10));
    int pushed = 0;
    for (int i = 0; i < n; i++) {
        if (st->manifest.count >= st->manifest.cap) break;
        cargo_unit_t unit = {0};
        if (!hash_legacy_migrate_unit(origin, c, (uint16_t)i, &unit)) continue;
        if (!station_manifest_push_with_chain(st, &unit, NULL)) break;
        pushed++;
    }
    return pushed;
}

static void hauler_sync_cargo_from_manifest(npc_ship_t *npc, const ship_t *ship) {
    if (!npc || !ship) return;
    memset(npc->cargo, 0, sizeof(npc->cargo));
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (u->commodity < COMMODITY_COUNT)
            npc->cargo[u->commodity] += 1.0f;
    }
}

static int hauler_load_station_units_for_contract(world_t *w, int npc_slot,
                                                  station_t *src, ship_t *dst,
                                                  const contract_t *contract,
                                                  int n) {
    if (!w || !src || !dst || !contract || n <= 0) return 0;
    if (contract->commodity < COMMODITY_RAW_ORE_COUNT ||
        contract->commodity >= COMMODITY_COUNT) return 0;
    if (src->manifest.cap == 0 && !station_manifest_bootstrap(src)) return 0;
    if (!ship_manifest_bootstrap(dst)) return 0;
    if (dst->manifest.count >= dst->manifest.cap) return 0;
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return 0;

    npc_ship_t *npc = &w->npc_ships[npc_slot];
    uint8_t npc_pubkey[32];
    npc_custody_pubkey(npc, npc_slot, npc_pubkey);

    int moved = 0;
    while (moved < n) {
        if (dst->manifest.count >= dst->manifest.cap) break;
        int idx = -1;
        for (uint16_t i = 0; i < src->manifest.count; i++) {
            if (contract_fit_is_ok(contract_fit_cargo_unit(contract,
                                                           &src->manifest.units[i]))) {
                idx = (int)i;
                break;
            }
        }
        if (idx < 0) break;
        cargo_unit_t unit = {0};
        cargo_receipt_chain_t chain = {0};
        if (!station_manifest_remove_with_chain(src, (uint16_t)idx,
                                                &unit, &chain)) {
            break;
        }
        cargo_receipt_chain_t outgoing = chain;
        (void)append_station_transfer_receipt(w, src, src->station_pubkey,
                                              npc_pubkey, &unit, &outgoing);
        if (!ship_manifest_push_with_chain(dst, &unit, &outgoing)) {
            (void)station_manifest_push_with_chain(src, &unit, &chain);
            break;
        }
        moved++;
    }
    return moved;
}

static int hauler_unload_ship_units_for_contract(world_t *w, int npc_slot,
                                                 ship_t *src, station_t *dst,
                                                 const contract_t *contract,
                                                 int n) {
    if (!w || !src || !dst || !contract || n <= 0) return 0;
    if (contract->commodity < COMMODITY_RAW_ORE_COUNT ||
        contract->commodity >= COMMODITY_COUNT) return 0;
    if (dst->manifest.cap == 0 && !station_manifest_bootstrap(dst)) return 0;
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return 0;

    npc_ship_t *npc = &w->npc_ships[npc_slot];
    uint8_t npc_pubkey[32];
    npc_custody_pubkey(npc, npc_slot, npc_pubkey);

    int moved = 0;
    while (moved < n) {
        if (dst->manifest.count >= dst->manifest.cap) break;
        int idx = -1;
        for (uint16_t i = 0; i < src->manifest.count; i++) {
            if (contract_fit_is_ok(contract_fit_cargo_unit(contract,
                                                           &src->manifest.units[i]))) {
                idx = (int)i;
                break;
            }
        }
        if (idx < 0) break;
        cargo_unit_t unit = {0};
        cargo_receipt_chain_t chain = {0};
        if (!ship_manifest_remove_with_chain(src, (uint16_t)idx,
                                             &unit, &chain)) {
            break;
        }
        cargo_receipt_chain_t incoming = chain;
        (void)append_station_transfer_receipt(w, dst, npc_pubkey,
                                              dst->station_pubkey, &unit,
                                              &incoming);
        if (!station_manifest_push_with_chain(dst, &unit, &incoming)) {
            (void)ship_manifest_push_with_chain(src, &unit, &chain);
            break;
        }
        moved++;
    }
    return moved;
}

static bool station_smelt_pair_for_ore(const station_t *st, commodity_t ore,
                                       vec2 *drop_point) {
    if (!st || ore == COMMODITY_COUNT) return false;
    bool found = false;
    float best_d = 1e18f;
    for (int fm = 0; fm < st->module_count; fm++) {
        const station_module_t *f = &st->modules[fm];
        if (f->type != MODULE_FURNACE) continue;
        if (f->scaffold) continue;
        if (module_instance_input_ore(f) != ore) continue;

        int ring = (int)f->ring;
        vec2 furnace_pos = module_world_pos_ring(st, ring, f->slot);
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int hm = 0; hm < st->module_count; hm++) {
                const station_module_t *h = &st->modules[hm];
                if (h->ring != adj) continue;
                if (h->scaffold) continue;
                if (h->type != MODULE_HOPPER) continue;
                if ((commodity_t)h->commodity != ore) continue;
                vec2 hopper_pos = module_world_pos_ring(st, adj, h->slot);
                float d = v2_dist_sq(furnace_pos, hopper_pos);
                if (d < best_d) {
                    best_d = d;
                    if (drop_point)
                        *drop_point = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
                    found = true;
                }
            }
        }
    }
    return found;
}

static bool station_smelt_pair_for_fragment(const station_t *st,
                                            int station_idx,
                                            const asteroid_t *fragment,
                                            vec2 *drop_point) {
    if (!st || !fragment || fragment->commodity == COMMODITY_COUNT) return false;
    bool found = false;
    float best_d = 1e18f;
    for (int fm = 0; fm < st->module_count; fm++) {
        const station_module_t *f = &st->modules[fm];
        if (f->type != MODULE_FURNACE) continue;
        if (f->scaffold) continue;
        if (module_instance_input_ore(f) != fragment->commodity) continue;
        if (fragment->commodity == COMMODITY_CRYSTAL_ORE &&
            fragment->crystal_stage == CRYSTAL_STAGE_INTERMEDIATE &&
            fragment->crystal_stage_station != 0xFFu &&
            fragment->crystal_stage_module != 0xFFu &&
            fragment->crystal_stage_station == (uint8_t)station_idx &&
            fragment->crystal_stage_module == (uint8_t)fm) {
            continue;
        }

        int ring = (int)f->ring;
        vec2 furnace_pos = module_world_pos_ring(st, ring, f->slot);
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int hm = 0; hm < st->module_count; hm++) {
                const station_module_t *h = &st->modules[hm];
                if (h->ring != adj) continue;
                if (h->scaffold) continue;
                if (h->type != MODULE_HOPPER) continue;
                if ((commodity_t)h->commodity != fragment->commodity) continue;
                vec2 hopper_pos = module_world_pos_ring(st, adj, h->slot);
                float d = v2_dist_sq(furnace_pos, hopper_pos);
                if (d < best_d) {
                    best_d = d;
                    if (drop_point)
                        *drop_point = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
                    found = true;
                }
            }
        }
    }
    return found;
}

static bool npc_home_has_smelt_endpoint(const world_t *w, const npc_ship_t *npc,
                                        commodity_t ore, vec2 *drop_point) {
    if (!w || !npc) return false;
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS) return false;
    return station_smelt_pair_for_ore(&w->stations[npc->home_station], ore,
                                      drop_point);
}

static bool npc_home_has_smelt_endpoint_for_fragment(const world_t *w,
                                                     const npc_ship_t *npc,
                                                     const asteroid_t *fragment,
                                                     vec2 *drop_point) {
    if (!w || !npc) return false;
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS) return false;
    return station_smelt_pair_for_fragment(&w->stations[npc->home_station],
                                           npc->home_station, fragment,
                                           drop_point);
}

/* ================================================================== */
/* NPC ships                                                          */
/* ================================================================== */

/* #294 Slice 6: paired character_t lifecycle.
 *
 * Each active NPC gets a paired character_t entry; future slices flip
 * the source-of-truth for brain state and damage routing onto it.
 * `ship_idx` carries the NPC slot during the transition — once the
 * unified ships[] pool lands, it'll point there instead.
 *
 * Nothing reads the character pool yet. These writes are intentionally
 * "dead" so the lifecycle is observable in saves/wire without flipping
 * any readers in the same slice. */
static character_kind_t character_kind_from_role(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return CHARACTER_KIND_NPC_MINER;
    case NPC_ROLE_HAULER: return CHARACTER_KIND_NPC_HAULER;
    case NPC_ROLE_TOW:    return CHARACTER_KIND_NPC_TOW;
    default:              return CHARACTER_KIND_NONE;
    }
}

static hull_class_t npc_hull_class_for_role(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return HULL_CLASS_MINER;
    case NPC_ROLE_HAULER: return HULL_CLASS_HAULER;
    case NPC_ROLE_TOW:    return HULL_CLASS_MINER;
    default:              return HULL_CLASS_MINER;
    }
}

static void npc_enforce_role_hull(npc_ship_t *npc) {
    if (!npc) return;
    npc->ship.hull_class = npc_hull_class_for_role(npc->role);
}

/* Find a free ships[] slot — one not pointed to by any active character.
 * Returns -1 if the pool is full. */
static int ship_pool_alloc_slot(const world_t *w) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int s = 0; s < MAX_SHIPS; s++) {
        bool taken = false;
        for (int i = 0; i < cap; i++) {
            if (w->characters[i].active && w->characters[i].ship_idx == s) {
                taken = true;
                break;
            }
        }
        if (!taken) return s;
    }
    return -1;
}

/* Initialize a ships[] slot from an NPC's snapshot. Frees any prior
 * manifest the slot was holding so this is safe for a slot that was
 * previously occupied (e.g. after rebuild_characters_from_npcs). */
static void ship_pool_init_from_npc(ship_t *ship, const npc_ship_t *npc) {
    ship_cleanup(ship);
    memset(ship, 0, sizeof(*ship));
    (void)ship_manifest_bootstrap(ship);
    ship->pos = npc->ship.pos;
    ship->vel = npc->ship.vel;
    ship->angle = npc->ship.angle;
    ship->hull_class = npc->ship.hull_class;
    ship->hull = npc->hull;
}

static int character_alloc_for_npc(world_t *w, int npc_slot, const npc_ship_t *npc) {
    int ship_slot = ship_pool_alloc_slot(w);
    if (ship_slot < 0) return -1;
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (w->characters[i].active) continue;
        character_t *c = &w->characters[i];
        memset(c, 0, sizeof(*c));
        c->active = true;
        c->kind = character_kind_from_role(npc->role);
        c->ship_idx = ship_slot;
        c->npc_slot = npc_slot;
        c->state = npc->state;
        c->target_asteroid = npc->target_asteroid;
        c->home_station = npc->home_station;
        c->dest_station = npc->dest_station;
        c->state_timer = npc->state_timer;
        c->towed_fragment = npc->towed_fragment;
        c->towed_scaffold = npc->towed_scaffold;
        ship_pool_init_from_npc(&w->ships[ship_slot], npc);
        return i;
    }
    /* No free character slot; release the ship slot we reserved. */
    ship_cleanup(&w->ships[ship_slot]);
    memset(&w->ships[ship_slot], 0, sizeof(w->ships[ship_slot]));
    return -1;
}

/* Find the paired character for an NPC slot, or -1. Matches on the
 * explicit npc_slot field — distinct from ship_idx which addresses a
 * different pool. */
static int character_for_npc_slot(const world_t *w, int npc_slot) {
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return -1;
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        const character_t *c = &w->characters[i];
        if (!c->active) continue;
        if (c->npc_slot != npc_slot) continue;
        if (c->kind != CHARACTER_KIND_NPC_MINER &&
            c->kind != CHARACTER_KIND_NPC_HAULER &&
            c->kind != CHARACTER_KIND_NPC_TOW) continue;
        return i;
    }
    return -1;
}

static void character_free_for_npc(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return;
    int ship_slot = w->characters[idx].ship_idx;
    if (ship_slot >= 0 && ship_slot < MAX_SHIPS) {
        ship_cleanup(&w->ships[ship_slot]);
        memset(&w->ships[ship_slot], 0, sizeof(w->ships[ship_slot]));
    }
    w->characters[idx].active = false;
}

/* Resolve an NPC slot to its paired ship, or NULL if no character is
 * paired or ship_idx is out of range. */
static ship_t *npc_ship_for(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return NULL;
    int s = w->characters[idx].ship_idx;
    if (s < 0 || s >= MAX_SHIPS) return NULL;
    return &w->ships[s];
}

/* Public wrapper: rejects NULL world and out-of-range / inactive
 * slots. Used by tests and external readers that want to inspect or
 * mutate an NPC's paired ship_t. */
ship_t *world_npc_ship_for(world_t *w, int npc_slot) {
    if (!w) return NULL;
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return NULL;
    if (!w->npc_ships[npc_slot].active) return NULL;
    return npc_ship_for(w, npc_slot);
}

/* End-of-tick paired-pool sync — npc -> ship for physics fields plus
 * ship -> npc for hull. Slice 13's pre-mirror (mirror_ship_pos_to_npc,
 * called at the top of each NPC step) is what makes external ship.pos
 * /vel/angle writes between ticks survive; after that pull, this
 * end-of-tick mirror just round-trips them back to the ship.
 *
 *   - hull is ship-authoritative since Slice 9/11 (apply_npc_ship_damage,
 *     hauler dock auto-repair both write ship.hull). Push ship -> npc
 *     so the npc-side despawn check reads a fresh value next tick.
 *   - pos / vel / angle / thrusting still get integrated on npc fields
 *     by the existing dispatch; npc -> ship at end of tick keeps the
 *     ship faithful for external readers and parity tests. Slice 14
 *     will collapse this into a single direction once npc_ship_t loses
 *     its physics fields. */
static void mirror_ship_to_npc(world_t *w, int npc_slot) {
    ship_t *s = npc_ship_for(w, npc_slot);
    if (!s) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    npc->hull = s->hull;
    s->pos = npc->ship.pos;
    s->vel = npc->ship.vel;
    s->angle = npc->ship.angle;
}

/* Slice 13: pre-mirror at the top of each NPC step. Pulls any external
 * ship.pos/vel/angle writes (PvP rock impulse, future autopilot, etc.)
 * into the npc fields BEFORE physics integrates this tick. Without
 * this, the post-mirror at end-of-tick would clobber the external
 * write with the integrated-from-stale-npc value — that was the bug
 * the parity tripwire (Slice 13a) was set up to surface. */
static void mirror_ship_pos_to_npc(world_t *w, int npc_slot) {
    ship_t *s = npc_ship_for(w, npc_slot);
    if (!s) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    npc->ship.pos = s->pos;
    npc->ship.vel = s->vel;
    npc->ship.angle = s->angle;
}

/* Apply damage to an NPC with optional kill attribution. The reverse
 * mirror at end-of-tick pushes the result into npc->hull so the
 * existing despawn check fires when hull <= 0. If the hit drops hull
 * to <= 0 AND killer_token is non-zero, emits SIM_EVENT_NPC_KILL.
 *
 * Public: external code (rock-throw collision, PvP, etc.) reaches NPC
 * damage through this helper so the unified ship_t.hull stays the
 * single source of truth. Validates inputs — out-of-range or inactive
 * slots are no-ops. */
void apply_npc_ship_damage_attributed(world_t *w, int npc_slot, float dmg,
                                       const uint8_t killer_token[8], uint8_t cause) {
    if (!w) return;
    if (dmg <= 0.0f) return;
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    if (!npc->active) return;
    ship_t *s = npc_ship_for(w, npc_slot);
    float prev_hull;
    if (!s) {
        prev_hull = npc->hull;
        npc->hull -= dmg;
        if (npc->hull < 0.0f) npc->hull = 0.0f;
    } else {
        prev_hull = s->hull;
        s->hull -= dmg;
        if (s->hull < 0.0f) s->hull = 0.0f;
    }
    /* Kill-feed: emit only on the lethal blow, only if attributed. */
    if (prev_hull > 0.0f) {
        float new_hull = s ? s->hull : npc->hull;
        if (new_hull <= 0.0f && killer_token) {
            bool nonzero = (killer_token[0] | killer_token[1] | killer_token[2] |
                            killer_token[3] | killer_token[4] | killer_token[5] |
                            killer_token[6] | killer_token[7]) != 0;
            if (nonzero) {
                sim_event_t kill_ev = {
                    .type = SIM_EVENT_NPC_KILL,
                    .npc_kill = {
                        .cause = cause,
                        .npc_role = (uint8_t)npc->role,
                    },
                };
                memcpy(kill_ev.npc_kill.killer_token, killer_token, 8);
                emit_event(w, kill_ev);
            }
        }
    }
}

/* Unattributed damage — environmental hits that don't credit a kill. */
void apply_npc_ship_damage(world_t *w, int npc_slot, float dmg) {
    apply_npc_ship_damage_attributed(w, npc_slot, dmg, NULL, DEATH_CAUSE_ASTEROID);
}

/* Mirror brain state from an NPC into its paired character_t (#294
 * Slice 7) AND physics state into its paired ship_t (#294 Slice 8).
 * Called at the top of each NPC's tick so future readers can trust the
 * controller + ship layer; the npc-side fields remain the source of
 * truth that the dispatch switch writes back to. */
static void mirror_npc_to_character(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return;
    const npc_ship_t *npc = &w->npc_ships[npc_slot];
    character_t *c = &w->characters[idx];
    c->state = npc->state;
    c->target_asteroid = npc->target_asteroid;
    c->home_station = npc->home_station;
    c->dest_station = npc->dest_station;
    c->state_timer = npc->state_timer;
    c->towed_fragment = npc->towed_fragment;
    c->towed_scaffold = npc->towed_scaffold;
    if (c->ship_idx >= 0 && c->ship_idx < MAX_SHIPS) {
        ship_t *s = &w->ships[c->ship_idx];
        s->pos = npc->ship.pos;
        s->vel = npc->ship.vel;
        s->angle = npc->ship.angle;
        s->hull_class = npc->ship.hull_class;
        /* Don't mirror hull npc->ship here: ship.hull is authoritative
         * (Slice 9 + 10). External callers — apply_npc_ship_damage and
         * future rock/PvP impact paths — may have mutated ship.hull
         * between this NPC's last tick and now; mirroring npc.hull
         * over it would lose that damage/repair. The end-of-tick
         * reverse mirror (mirror_ship_to_npc) keeps npc.hull in sync. */
    }
}

/* Target NPC roster per station. Starter station targets must match the
 * world_reset seed (see game_sim.c: spawn_npc calls). Active outposts get
 * a small local roster only when their modules expose useful work, so
 * frontier expansion can become self-supporting without inflating the
 * physical NPC pool beyond MAX_NPC_SHIPS. */
static void station_target_npc_counts(int station_idx, const station_t *st,
                                      int *miners, int *haulers, int *tows) {
    *miners = 0;
    *haulers = 0;
    *tows = 0;
    if (!st || !station_is_active(st)) return;
    switch (station_idx) {
    case 0: *miners = 2; *haulers = 2; return;  /* Prospect */
    case 1: /* Kepler */
        *miners = 0;
        *haulers = 1;
        *tows = station_has_module(st, MODULE_SHIPYARD) ? 1 : 0;
        return;
    case 2: /* Helios */
        *miners = 1;
        *haulers = 1;
        *tows = station_has_module(st, MODULE_SHIPYARD) ? 1 : 0;
        return;
    default:
        *miners = station_has_module(st, MODULE_FURNACE) ? 1 : 0;
        *haulers = station_has_module(st, MODULE_DOCK) ? 1 : 0;
        *tows = station_has_module(st, MODULE_SHIPYARD) ? 1 : 0;
        return;
    }
}

/* Walk the active NPC pool and count active members per home station,
 * per role. Used by replenish_npc_roster to pick the most-understaffed
 * (station, role) pair. */
static void count_npc_roster(const world_t *w,
                             int miners[MAX_STATIONS],
                             int haulers[MAX_STATIONS],
                             int tows[MAX_STATIONS]) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        miners[s] = 0;
        haulers[s] = 0;
        tows[s] = 0;
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        const npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS) continue;
        if (npc->role == NPC_ROLE_MINER)  miners[npc->home_station]++;
        if (npc->role == NPC_ROLE_HAULER) haulers[npc->home_station]++;
        if (npc->role == NPC_ROLE_TOW)    tows[npc->home_station]++;
    }
}

/* Spawn at most ONE NPC to fill the largest gap between actual and
 * target roster. Drip-feed (caller gates with npc_respawn_timer) so a
 * full wipe recovers gradually. Sovereign station can run negative; pool
 * is informational, so spawning is no longer gated on solvency.
 * Returns true if a spawn fired. */
static bool replenish_npc_roster(world_t *w) {
    int miners[MAX_STATIONS], haulers[MAX_STATIONS], tows[MAX_STATIONS];
    count_npc_roster(w, miners, haulers, tows);

    /* Find the largest shortfall across all (station, role) pairs.
     * Tie-broken by station index (lower wins). */
    int best_station = -1;
    npc_role_t best_role = NPC_ROLE_MINER;
    int best_shortfall = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        int target_m = 0, target_h = 0, target_t = 0;
        station_target_npc_counts(s, &w->stations[s], &target_m, &target_h, &target_t);
        /* Sovereign station can run negative; pool is informational. */
        int short_m = target_m - miners[s];
        int short_h = target_h - haulers[s];
        int short_t = target_t - tows[s];
        if (short_m > best_shortfall) {
            best_shortfall = short_m; best_station = s; best_role = NPC_ROLE_MINER;
        }
        if (short_h > best_shortfall) {
            best_shortfall = short_h; best_station = s; best_role = NPC_ROLE_HAULER;
        }
        if (short_t > best_shortfall) {
            best_shortfall = short_t; best_station = s; best_role = NPC_ROLE_TOW;
        }
    }
    if (best_station < 0) return false;
    int slot = spawn_npc(w, best_station, best_role);
    return slot >= 0;
}

void rebuild_characters_from_npcs(world_t *w) {
    /* Free heap-allocated manifests on all ships[] slots before we
     * deactivate the characters that pinned them. Without this, slots
     * that aren't reclaimed by the next pass (e.g. once MAX_SHIPS >
     * MAX_NPC_SHIPS or when fewer NPCs are active than before) leak
     * their manifest_t.units allocation. ship_pool_init_from_npc on
     * re-alloc would also clean — but only for slots actually picked. */
    for (int s = 0; s < MAX_SHIPS; s++) {
        ship_cleanup(&w->ships[s]);
        memset(&w->ships[s], 0, sizeof(w->ships[s]));
    }
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (w->characters[i].kind == CHARACTER_KIND_NPC_MINER ||
            w->characters[i].kind == CHARACTER_KIND_NPC_HAULER ||
            w->characters[i].kind == CHARACTER_KIND_NPC_TOW) {
            w->characters[i].active = false;
        }
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* v32 -> v33 migration: regenerate session_token for any
         * active NPC loaded without one. Same byte layout as
         * spawn_npc so the token shape is consistent. */
        bool has_token = (npc->session_token[0] | npc->session_token[1] |
                          npc->session_token[2] | npc->session_token[3] |
                          npc->session_token[4] | npc->session_token[5] |
                          npc->session_token[6] | npc->session_token[7]) != 0;
        if (!has_token) {
            if (w->next_npc_token == 0) w->next_npc_token = 1;
            uint16_t tok = w->next_npc_token++;
            npc->session_token[0] = 'N';
            npc->session_token[1] = 'P';
            npc->session_token[2] = 'C';
            npc->session_token[3] = (uint8_t)npc->home_station;
            npc->session_token[4] = (uint8_t)npc->role;
            npc->session_token[5] = (uint8_t)n;
            npc->session_token[6] = (uint8_t)(tok & 0xFF);
            npc->session_token[7] = (uint8_t)((tok >> 8) & 0xFF);
        }
        npc_enforce_role_hull(npc);
        (void)character_alloc_for_npc(w, n, npc);
    }
}

/* Spawn an NPC at a station. Returns slot index or -1 if full. */
int spawn_npc(world_t *w, int station_idx, npc_role_t role) {
    int slot = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w->npc_ships[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    station_t *st = &w->stations[station_idx];
    hull_class_t hc = npc_hull_class_for_role(role);
    npc_ship_t *npc = &w->npc_ships[slot];
    memset(npc, 0, sizeof(*npc));
    /* Clear stale path from previous occupant of this slot. */
    *nav_npc_path(slot) = (nav_path_t){0};
    npc->active = true;
    npc->role = role;
    npc->ship.hull_class = hc;
    npc->state = NPC_STATE_DOCKED;
    npc->ship.pos = v2_add(st->pos, v2(30.0f * (float)(slot % 3 - 1), -(st->radius + hull_def_for_class(hc)->ship_radius + 50.0f)));
    npc->ship.angle = PI_F * 0.5f;
    npc->target_asteroid = -1;
    npc->towed_fragment = -1;
    npc->towed_scaffold = -1;
    npc->home_station = station_idx;
    npc->dest_station = station_idx;
    npc->state_timer = (role == NPC_ROLE_MINER) ? NPC_DOCK_TIME : HAULER_DOCK_TIME;
    npc->hull = npc_max_hull(npc);
    npc->tint_r = 1.0f; npc->tint_g = 1.0f; npc->tint_b = 1.0f;
    /* Tow drones get a distinct yellow-amber tint */
    if (role == NPC_ROLE_TOW) {
        npc->tint_r = 1.0f; npc->tint_g = 0.85f; npc->tint_b = 0.30f;
    }
    /* Per-NPC economic identity. Bytes:
     *   [0..2] 'NPC' magic (distinguishes from player session_tokens
     *          which are 8 random bytes from the session handshake)
     *   [3]    station_idx
     *   [4]    role
     *   [5]    slot
     *   [6..7] world counter (little-endian) — increments each spawn
     *          so respawns of the same role at the same slot get a
     *          fresh ledger identity. The dead token's ledger entry
     *          stays attributed until the 16-slot LRU evicts it. */
    if (w->next_npc_token == 0) w->next_npc_token = 1;
    uint16_t tok = w->next_npc_token++;
    npc->session_token[0] = 'N';
    npc->session_token[1] = 'P';
    npc->session_token[2] = 'C';
    npc->session_token[3] = (uint8_t)station_idx;
    npc->session_token[4] = (uint8_t)role;
    npc->session_token[5] = (uint8_t)slot;
    npc->session_token[6] = (uint8_t)(tok & 0xFF);
    npc->session_token[7] = (uint8_t)((tok >> 8) & 0xFF);
    /* No starter balance — fresh NPCs run on credit and pay it back
     * as they complete deliveries. ledger_force_debit at the dock
     * lets the balance go negative; the chain self-balances over
     * time as the hauler ferries goods. */
    /* Pair a character_t with the NPC. Lifecycle-only — nothing reads
     * it yet (#294 Slice 6). If the pool is somehow exhausted we still
     * spawn the NPC; this is best-effort during the transition. */
    (void)character_alloc_for_npc(w, slot, npc);
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_NPC_SPAWNED,
        .npc_spawned = { .slot = slot, .role = role, .home_station = station_idx },
    });
    SIM_LOG("[sim] spawned %s at station %d (slot %d)\n",
            role == NPC_ROLE_MINER ? "miner" :
            role == NPC_ROLE_HAULER ? "hauler" : "tow drone",
            station_idx, slot);
    return slot;
}

static bool npc_target_valid(const world_t *w, const npc_ship_t *npc) {
    if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_ASTEROIDS) return false;
    const asteroid_t *a = &w->asteroids[npc->target_asteroid];
    if (!a->active || a->tier == ASTEROID_TIER_S) return false;
    return a->tier >= max_mineable_tier(npc->ship.mining_level);
}

/* Asteroid-already-taken check, reading from the controller layer
 * (#294 Slice 7+8): scan characters[] for any other MINER targeting
 * `target_idx`. The mirror at top of tick keeps character.target_asteroid
 * in sync with npc.target_asteroid. `self_char_idx` is excluded so the
 * caller doesn't see itself as a competitor. */
static bool miner_target_taken(const world_t *w, int target_idx, int self_char_idx) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (i == self_char_idx) continue;
        const character_t *c = &w->characters[i];
        if (!c->active || c->kind != CHARACTER_KIND_NPC_MINER) continue;
        if (c->target_asteroid == target_idx) return true;
    }
    return false;
}

/* Look for a free-floating S-tier fragment within `range_sq` of the
 * NPC. "Free" means not currently on any player's tractor (their
 * ship.towed_fragments[] list) and not already claimed by another
 * miner NPC (their npc_ship_t.towed_fragment).
 *
 * Used so miner NPCs prefer cleaning up loose fragments over fracturing
 * fresh rock. Returns asteroid index, or -1. */
static int npc_find_loose_fragment(const world_t *w, const npc_ship_t *self, float range_sq) {
    int best = -1;
    float tractor_r = ship_tractor_range(&self->ship);
    if (tractor_r <= 0.0f) return -1;
    float tractor_sq = tractor_r * tractor_r;
    if (range_sq <= 0.0f || range_sq > tractor_sq) range_sq = tractor_sq;
    float best_d = range_sq;
    int self_slot = (int)(self - w->npc_ships);
    const station_t *home = (self->home_station >= 0 && self->home_station < MAX_STATIONS)
                          ? &w->stations[self->home_station]
                          : NULL;
    if (!home) return -1;
    for (int fi = 0; fi < MAX_ASTEROIDS; fi++) {
        const asteroid_t *f = &w->asteroids[fi];
        if (!f->active || f->tier != ASTEROID_TIER_S) continue;
        if (!npc_home_has_smelt_endpoint_for_fragment(w, self, f, NULL)) continue;
        if (station_raw_ore_need_score(home, f->commodity) <= 0.0f) continue;
        bool taken = false;
        /* Player tow check. */
        for (int p = 0; p < MAX_PLAYERS && !taken; p++) {
            if (!w->players[p].connected) continue;
            const ship_t *ship = &w->players[p].ship;
            for (int t = 0; t < (int)(sizeof(ship->towed_fragments) / sizeof(ship->towed_fragments[0])); t++) {
                if (ship->towed_fragments[t] == (int16_t)fi) { taken = true; break; }
            }
        }
        if (taken) continue;
        /* Other-NPC tow check. */
        for (int j = 0; j < MAX_NPC_SHIPS; j++) {
            if (j == self_slot) continue;
            if (w->npc_ships[j].active && w->npc_ships[j].towed_fragment == fi) {
                taken = true; break;
            }
        }
        if (taken) continue;
        float d2 = v2_dist_sq(self->ship.pos, f->pos);
        if (d2 < best_d) { best_d = d2; best = fi; }
    }
    return best;
}

/* Wrapper: claim a loose fragment for this NPC and stamp the
 * smelt-payout token. Called at every place the miner is about to
 * decide on a fracture target — picking up an existing fragment is
 * always preferable to fracturing more rock. Returns true iff a
 * fragment was claimed and the caller should transition to
 * NPC_STATE_RETURN_TO_STATION. */
/* NPC tow tokens start with the literal "NPC" prefix (see spawn_npc).
 * A player's session_token is 8 random bytes — collision probability
 * with the "NPC\0" prefix is ~1/16M and the worst case is one fragment
 * smelt going to the wrong ledger; cheaper than a side-table. Used by
 * the NPC tow paths to AVOID overwriting a player's stamp on a
 * fragment they already towed (first-player-tower wins). */
static bool token_is_npc(const uint8_t token[8]) {
    return token && token[0] == 'N' && token[1] == 'P' && token[2] == 'C';
}

static bool npc_try_claim_loose_fragment(world_t *w, npc_ship_t *npc, float range_sq) {
    int frag = npc_find_loose_fragment(w, npc, range_sq);
    if (frag < 0) return false;
    npc->towed_fragment = frag;
    asteroid_t *f = &w->asteroids[frag];
    /* Only stamp when the slot is empty or carries another NPC's
     * token — never overwrite a player tow stamp. */
    bool stamped = false;
    for (int b = 0; b < 8 && !stamped; b++) if (f->last_towed_token[b]) stamped = true;
    if (!stamped || token_is_npc(f->last_towed_token)) {
        memcpy(f->last_towed_token, npc->session_token, sizeof(f->last_towed_token));
    }
    return true;
}

/* True if every ore the miner can deliver is currently pointless:
 * either the raw hopper is above target, the refined output has no
 * room, or the downstream module product is stocked. This keeps miners
 * from adding fresh fragments to a saturated local chain while still
 * letting multi-ore stations work on a starved ore even when another
 * hopper/product is healthy. */
static bool npc_home_has_no_ore_need(const world_t *w, const npc_ship_t *npc) {
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS) return false;
    const station_t *home = &w->stations[npc->home_station];
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
        if (!station_smelt_pair_for_ore(home, (commodity_t)c, NULL)) continue;
        if (station_raw_ore_need_score(home, (commodity_t)c) > 0.0f)
            return false;
    }
    return true;
}

static int npc_find_mineable_asteroid(const world_t *w, const npc_ship_t *npc) {
    int self_npc_slot = (int)(npc - w->npc_ships);
    int self_char = character_for_npc_slot(w, self_npc_slot);
    asteroid_tier_t max_tier = max_mineable_tier(npc->ship.mining_level);

    /* Priority: DESTROY contract targets first — but only if reasonably
     * nearby. Without the distance cap, a Helios miner would pick up a
     * FRACTURE distress posted near Prospect and drift halfway across
     * the map "lasering" a rock it can't reach. 2500u is roughly the
     * radius at which a miner would notice trouble in its own sector. */
    const float MAX_DISTRESS_DIST_SQ = 2500.0f * 2500.0f;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active || w->contracts[k].action != CONTRACT_FRACTURE) continue;
        int idx = w->contracts[k].target_index;
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
        if (w->asteroids[idx].tier < max_tier) continue;
        if (v2_dist_sq(npc->ship.pos, w->asteroids[idx].pos) > MAX_DISTRESS_DIST_SQ) continue;
        if (!miner_target_taken(w, idx, self_char)) return idx;
    }

    /* Most-needed useful rock: the home station must expose a concrete
     * furnace+hopper endpoint for the ore, and the ore must feed a
     * non-saturated downstream chain. Distance only breaks ties within
     * the same demand band; otherwise Helios keeps mining nearby
     * crystal while the laser line is actually starved for cuprite. */
    const station_t *home = (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
                          ? &w->stations[npc->home_station]
                          : NULL;
    if (!home) return -1;
    int best = -1;
    float best_need = 0.0f;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        if (a->tier < max_tier) continue;
        if (signal_npc_confidence(signal_strength_at(w, a->pos)) < 0.1f) continue;
        if (miner_target_taken(w, i, self_char)) continue;
        if (!station_smelt_pair_for_ore(home, a->commodity, NULL)) continue;
        float need = station_raw_ore_need_score(home, a->commodity);
        if (need <= 0.0f) continue;
        float d = v2_dist_sq(npc->ship.pos, a->pos);
        if (need > best_need + 0.05f ||
            (fabsf(need - best_need) <= 0.05f && d < best_d)) {
            best_need = need;
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* Forward decl — definition below; npc_steer_toward routes through it. */
static void npc_apply_flight_cmd(npc_ship_t *npc, flight_cmd_t cmd, float dt);
static bool npc_point_inside_station_nav_envelope(const station_t *st, vec2 p);

/* Direct face-and-thrust steering used by the MINING-approach state.
 * Produces a normalized flight_cmd_t and routes through
 * npc_apply_flight_cmd so all NPC physics goes through the same
 * sim_ship primitives as the path-following steer. */
static void npc_steer_toward(npc_ship_t *npc, vec2 target, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    vec2 delta = v2_sub(target, npc->ship.pos);
    float desired = atan2f(delta.y, delta.x);
    float diff = wrap_angle(desired - npc->ship.angle);
    float max_turn = hull->turn_speed * dt;
    flight_cmd_t cmd = {0.0f, 1.0f, false};
    if (max_turn > 0.0f) {
        float t = diff / max_turn;
        if (t > 1.0f) t = 1.0f;
        else if (t < -1.0f) t = -1.0f;
        cmd.turn = t;
    }
    npc_apply_flight_cmd(npc, cmd, dt);
}

/* (Reactive avoidance steering removed — all NPC/autopilot navigation
 * now uses A* paths via npc_steer_with_path. compute_path_avoidance
 * is retained for potential future use by manual-play collision hints.) */

/* Apply a normalized flight_cmd_t (turn/thrust each in -1..1) to an NPC,
 * routed through the shared sim_ship primitives. Caller still owns
 * physics integration (npc_apply_physics) and any thrust<0 handling
 * (e.g. hover-specific brake-away-from-target).
 *
 * Path-following negative thrust maps to the shared velocity brake.
 * Without that, haulers coast past station waypoints and orbit the
 * rotating dock lanes instead of slowing down enough to enter. To
 * throttle the engine (hauler-tow paths used to pass hull->accel *
 * 0.6f), scale cmd.thrust before calling — thrust ∈ [-1,1] so this is
 * equivalent to the old accel multiplier. */
/* Stamp the NPC's input intent — symmetric with sp->input on the
 * player path. The apply path reads from npc->input.{turn,thrust}
 * instead of taking a flight_cmd_t directly. NPCs and players share
 * the same input → physics pipeline; what differs is who fills the
 * struct (AI brain vs. keyboard sample). */
static void npc_set_intent(npc_ship_t *npc, flight_cmd_t cmd) {
    npc->input.turn = cmd.turn;
    npc->input.thrust = cmd.thrust;
}

static void npc_apply_flight_cmd(npc_ship_t *npc, flight_cmd_t cmd, float dt) {
    npc_set_intent(npc, cmd);
    step_ship_rotation(&npc->ship, dt, npc->input.turn);

    vec2 fwd = ship_forward(npc->ship.angle);
    step_ship_thrust(&npc->ship, dt, npc->input.thrust, fwd, /*boost=*/false, 0.0f,
                     /*reverse_allowed=*/false);

    npc->thrusting = npc->input.thrust > 0.0f;
}

/* A*-guided NPC steering via the shared flight controller. Creates a
 * temporary ship_t so flight_steer_to can read pos/vel/angle/hull_class
 * — slice 4 will fold ship_t into npc_ship_t and drop the view.
 *
 * `thrust_scale` ∈ (0,1] throttles forward thrust without changing the
 * controller's turn output; pass 1.0 for full engine, smaller values
 * for tow paths (was hull->accel * scale before). */
static void npc_steer_with_path(const world_t *w, int npc_idx, npc_ship_t *npc,
                                vec2 final_target, float thrust_scale, float dt) {
    nav_path_t *path = nav_npc_path(npc_idx);
    bool station_local = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        if (npc_point_inside_station_nav_envelope(st, npc->ship.pos) ||
            npc_point_inside_station_nav_envelope(st, final_target)) {
            station_local = true;
            break;
        }
    }
    if (station_local && path->age > 0.25f)
        nav_force_replan(path);
    float max_speed = station_local ? 110.0f : 200.0f;
    flight_cmd_t cmd = flight_steer_to(w, &npc->ship, path, final_target,
                                        0.0f, max_speed, dt);
    cmd.thrust *= thrust_scale;
    npc_apply_flight_cmd(npc, cmd, dt);
}

/* Drag + position integration + signal-frontier pushback, routed
 * through step_ship_motion via the ship_view+writeback adapter. The
 * NPC confidence-graduated push that used to live here was retired in
 * favor of the player frontier yank — NPCs and players now share the
 * same boundary behavior. NPC willingness to *operate* at low signal
 * is still gated separately by signal_npc_confidence() in the AI brain
 * (mining target selection, willingness to leave the dock); only the
 * physics-side pushback was duplicated. */
static void npc_apply_physics(npc_ship_t *npc, float dt, const world_t *w) {
    float sig = signal_strength_at(w, npc->ship.pos);
    step_ship_motion(&npc->ship, dt, w, sig);
}


/* NPC circle pushback: routed through the shared sim_ship primitive
 * on the embedded ship_t. NPCs take no damage from station geometry
 * today; if that changes, the impact return is available. */
static bool resolve_npc_circle(npc_ship_t *npc, vec2 center, float radius) {
    vec2 before = npc->ship.pos;
    (void)resolve_ship_circle_pushback(&npc->ship, center, radius);
    return v2_dist_sq(before, npc->ship.pos) > 0.001f;
}

/* NPC corridor collision: routed through the shared sim_ship
 * primitive on the embedded ship_t. Returns true on push so the
 * caller can force a nav replan. */
static bool resolve_npc_annular_sector(npc_ship_t *npc, vec2 center,
                                        float ring_r, float angle_a, float arc_delta) {
    vec2 before = npc->ship.pos;
    (void)resolve_ship_annular_pushback(&npc->ship, center, ring_r,
                                        angle_a, arc_delta);
    return v2_dist_sq(before, npc->ship.pos) > 0.001f;
}

static float npc_station_nav_envelope_radius(const station_t *st) {
    int max_ring = station_max_ring(st);
    float r = (max_ring >= 1 && max_ring <= STATION_NUM_RINGS)
        ? STATION_RING_RADIUS[max_ring] + 220.0f
        : st->dock_radius + 220.0f;
    return (r > st->dock_radius) ? r : st->dock_radius;
}

static bool npc_point_inside_station_nav_envelope(const station_t *st, vec2 p) {
    float r = npc_station_nav_envelope_radius(st);
    return v2_dist_sq(p, st->pos) <= r * r;
}

static int npc_station_entry_road_ring(const station_t *st) {
    int road_ring = station_max_ring(st);
    while (road_ring > 1 && ring_module_count(st, road_ring) <= 1)
        road_ring--;
    if (road_ring < 1 || road_ring > STATION_NUM_RINGS) return 0;
    if (!station_ring_open_gap_lane(st, road_ring, NULL, NULL)) return 0;
    return road_ring;
}

static vec2 npc_station_entry_inner_target(const station_t *st) {
    int road_ring = npc_station_entry_road_ring(st);
    if (road_ring <= 0) return station_entry_target(st, st->pos);
    float r = STATION_RING_RADIUS[road_ring] - 90.0f;
    if (r < st->radius + 90.0f) r = st->radius + 90.0f;
    return station_ring_open_gap_lane_pos(st, road_ring, r);
}

static vec2 npc_station_entry_stage_target(const station_t *st, vec2 ship_pos,
                                           vec2 want_target) {
    int outer_ring = station_max_ring(st);
    int road_ring = npc_station_entry_road_ring(st);
    if (outer_ring <= 1 || road_ring <= 0) return want_target;

    vec2 rel = v2_sub(ship_pos, st->pos);
    float ship_r = v2_len(rel);
    float target_r = v2_len(v2_sub(want_target, st->pos));
    if (target_r >= STATION_RING_RADIUS[outer_ring] + 20.0f)
        return want_target;

    vec2 outer = station_entry_target(st, ship_pos);
    if (ship_r > STATION_RING_RADIUS[outer_ring] + 80.0f &&
        v2_dist_sq(ship_pos, outer) > 140.0f * 140.0f) {
        return outer;
    }

    vec2 inner = npc_station_entry_inner_target(st);
    float inner_gate = STATION_RING_RADIUS[road_ring] - 70.0f;
    if (inner_gate < st->radius + 90.0f) inner_gate = st->radius + 90.0f;
    if (ship_r > inner_gate) {
        return inner;
    }

    return want_target;
}

/* Treat station rings as traffic envelopes. If an NPC is inside any
 * station and its desired target is outside that station, first route
 * to that station's dock exit. Conversely, if the desired target is an
 * internal station work point and the NPC is outside that station,
 * route to the outer roadway first. This covers haulers leaving a
 * non-home destination after unloading and miners returning to a
 * furnace/hopper midpoint with a fragment in tow. */
static vec2 npc_target_routed_through_station_docks(const world_t *w,
                                                    const npc_ship_t *npc,
                                                    vec2 want_target) {
    int exit_station = -1;
    float best_exit_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        bool ship_inside = npc_point_inside_station_nav_envelope(st, npc->ship.pos);
        bool target_inside = npc_point_inside_station_nav_envelope(st, want_target);
        if (!ship_inside || target_inside) continue;
        float d = v2_dist_sq(npc->ship.pos, st->pos);
        if (d < best_exit_d) {
            best_exit_d = d;
            exit_station = s;
        }
    }
    if (exit_station >= 0) {
        const station_t *st = &w->stations[exit_station];
        vec2 outer = station_entry_target(st, npc->ship.pos);
        if (v2_dist_sq(npc->ship.pos, outer) < 180.0f * 180.0f)
            return want_target;
        return station_exit_target(st, npc->ship.pos);
    }

    int entry_station = -1;
    float best_entry_d = 1e18f;
    vec2 best_entry_target = want_target;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        bool target_inside = npc_point_inside_station_nav_envelope(st, want_target);
        if (!target_inside) continue;
        vec2 staged_target = npc_station_entry_stage_target(st, npc->ship.pos,
                                                            want_target);
        if (v2_dist_sq(staged_target, want_target) < 1.0f) continue;
        float d = v2_dist_sq(want_target, st->pos);
        if (d < best_entry_d) {
            best_entry_d = d;
            entry_station = s;
            best_entry_target = staged_target;
        }
    }
    if (entry_station >= 0) {
        return best_entry_target;
    }

    return want_target;
}

static bool npc_reached_station_dock_lane(const npc_ship_t *npc,
                                          const station_t *st,
                                          vec2 dock_lane) {
    float lane_r = 90.0f;
    if (v2_dist_sq(npc->ship.pos, dock_lane) < lane_r * lane_r) return true;

    if (station_max_ring(st) > 1 && v2_len(npc->ship.vel) < 130.0f) {
        vec2 outer = station_entry_target(st, npc->ship.pos);
        if (v2_dist_sq(npc->ship.pos, outer) < 130.0f * 130.0f)
            return true;
        vec2 inner = npc_station_entry_inner_target(st);
        if (v2_dist_sq(npc->ship.pos, inner) < 130.0f * 130.0f)
            return true;
    }
    return false;
}

static void npc_resolve_station_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    float ship_r = hull->ship_radius;
    bool any_push = false;
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        if (!station_collides(st)) continue;

        station_geom_t geom;
        station_build_geom(st, &geom);

        /* Core: empty space, no collision */

        /* Module circles */
        for (int ci = 0; ci < geom.circle_count; ci++) {
            if (resolve_npc_circle(npc, geom.circles[ci].center,
                                   geom.circles[ci].radius)) {
                any_push = true;
            }
        }

        /* Near-module suppression + corridor annular sectors
         * (matches player collision logic) */
        float npc_dist = sqrtf(v2_dist_sq(npc->ship.pos, st->pos));
        vec2 npc_delta = v2_sub(npc->ship.pos, st->pos);
        float npc_ang = atan2f(npc_delta.y, npc_delta.x);

        for (int ci = 0; ci < geom.corridor_count; ci++) {
            float ring_r = geom.corridors[ci].ring_radius;

            /* Check if NPC is near any module on this corridor's ring */
            bool near_module = false;
            if (fabsf(npc_dist - ring_r) < STATION_CORRIDOR_HW + ship_r + STATION_MODULE_COL_RADIUS) {
                for (int mi = 0; mi < geom.circle_count; mi++) {
                    if (geom.circles[mi].ring != geom.corridors[ci].ring) continue;
                    float ang_diff = wrap_angle(npc_ang - geom.circles[mi].angle);
                    float angular_size = (ring_r > 1.0f) ? (STATION_MODULE_COL_RADIUS + ship_r) / ring_r : 0.0f;
                    if (fabsf(ang_diff) < angular_size) {
                        near_module = true;
                        break;
                    }
                }
            }

            if (!near_module) {
                if (resolve_npc_annular_sector(npc, geom.center,
                        ring_r, geom.corridors[ci].angle_a, geom.corridors[ci].arc_delta)) {
                    any_push = true;
                }
            }
        }
    }
    /* Any corridor push means our cached A* path is leading us into
     * walls — force a replan so the next tick's flight_steer_to picks
     * a fresh route around the obstacle. Without this the NPC parks
     * against the wall, since path-following keeps pointing at the
     * same target and the forward-clearance brake bottoms out. */
    if (any_push) {
        int slot = (int)(npc - w->npc_ships);
        if (slot >= 0 && slot < MAX_NPC_SHIPS) {
            nav_force_replan(nav_npc_path(slot));
        }
    }
}

static void npc_resolve_asteroid_collisions(world_t *w, npc_ship_t *npc) {
    int towed = npc->towed_fragment;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (i == towed) continue;  /* tow physics owns this fragment */
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        /* Fragments (collectible-tier) collide too — a thrown or
         * tractored ore chunk should hit an NPC the same way it hits a
         * player. Geometry + mass-equal bounce live in sim_ship; only
         * NPC-specific damage routing layers on top. Unconditional
         * writeback so the geometric push-out lands even when the
         * contact is separating (impact=0, no damage but ship was
         * still moved out of overlap). */
        float impact = resolve_ship_asteroid_pushback(&npc->ship, a);
        if (impact <= 0.0f) continue;

        bool attributed = asteroid_is_ballistic(a);
        uint8_t thrown_token[8] = {0};
        if (attributed) {
            memcpy(thrown_token, a->thrown_by_token, sizeof(thrown_token));
            asteroid_clear_thrown(a);
        }

        float size_mult = a->radius / 30.0f;
        if (size_mult < 0.5f) size_mult = 0.5f;
        if (size_mult > 2.5f) size_mult = 2.5f;
        float dmg = collision_damage_for(impact, size_mult);
        if (dmg <= 0.0f) continue;
        int npc_slot = (int)(npc - w->npc_ships);
        if (attributed) {
            apply_npc_ship_damage_attributed(w, npc_slot, dmg,
                thrown_token, DEATH_CAUSE_THROWN_ROCK);
        } else {
            apply_npc_ship_damage(w, npc_slot, dmg);
        }
    }
}

/* Find nearest active station with a dock module. Returns 0 as fallback. */
static int nearest_active_dock_station(const world_t *w, vec2 pos) {
    int best = 0;
    float best_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_is_active(&w->stations[s])) continue;
        if (!station_has_module(&w->stations[s], MODULE_DOCK)) continue;
        float d = v2_dist_sq(pos, w->stations[s].pos);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}

static void npc_validate_stations(world_t *w, npc_ship_t *npc) {
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS ||
        !station_is_active(&w->stations[npc->home_station]))
        npc->home_station = nearest_active_dock_station(w, npc->ship.pos);
    if (npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS)
        npc->dest_station = npc->home_station;
    /* Tow drones can deliver to planned stations (blueprints) which are
     * not active yet. Only reset dest for non-tow roles. */
    else if (npc->role != NPC_ROLE_TOW &&
             !station_is_active(&w->stations[npc->dest_station]))
        npc->dest_station = npc->home_station;
}

static void step_hauler(world_t *w, npc_ship_t *npc, int n, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    ship_t *hauler_ship = npc_ship_for(w, n);
    if (hauler_ship && hauler_ship->manifest.count > 0)
        hauler_sync_cargo_from_manifest(npc, hauler_ship);
    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->ship.vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            /* Dock-contact gossip: bidirectional set-union with the
             * home station. After this, npc->known_contracts holds
             * everything home knows (its own issued contracts plus
             * intel from prior visiting ships), capped by FIFO. */
            gossip_dock_handshake(w, npc->home_station,
                                  npc->known_contracts,
                                  &npc->known_contract_count,
                                  SHIP_KNOWN_CONTRACT_CAP,
                                  &npc->knowledge);

            station_t *home = &w->stations[npc->home_station];
            float carried = 0.0f;
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) carried += npc->cargo[c];
            float space = hull->ingot_capacity - carried;

            /* Contract-driven routing: scan only the NPC's own bounded
             * memory of contract summaries — no peer-station radio.
             * SKIP contracts whose station_index is our own home —
             * a self-delivery is zero-distance, scores dist=1 = max
             * possible price/dist, and would always win the race. The
             * bug it caused: Prospect issues a P6 kit-import contract
             * for itself; Prospect haulers loaded local stock and
             * "delivered" it back to Prospect, never carrying ferrite
             * ingots out to Kepler. */
            int best_known = -1;
            contract_t *best_contract = NULL;
            float best_score = 0.0f;
            for (int k = 0; k < npc->known_contract_count; k++) {
                const contract_summary_t *cs = &npc->known_contracts[k];
                if (!cs->active) continue;
                if (cs->action != CONTRACT_TRACTOR) continue;
                if (cs->station_index >= MAX_STATIONS) continue;
                if (cs->station_index == npc->home_station) continue;
                commodity_t c = (commodity_t)cs->commodity;
                if (c < COMMODITY_RAW_ORE_COUNT) continue; /* raw ore moves as fragments */
                contract_t *ct = hauler_pickup_contract_from_summary(
                    w, cs, &home->manifest);
                if (!ct) continue;
                int fit_stock = contract_fit_manifest_count(ct, &home->manifest);
                int takeable = fit_stock - hauler_reserve_units();
                if (takeable <= 0)
                    continue; /* no manifest-backed stock at home to fill */
                /* Stations are physical landmarks visible via the signal
                 * grid — position lookup is the legal infrastructure
                 * gossip channel, not radio. */
                float dist = fmaxf(1.0f,
                    v2_len(v2_sub(w->stations[cs->station_index].pos, home->pos)));
                float score = contract_price(ct) / dist;
                if (score > best_score) {
                    best_score = score;
                    best_known = k;
                    best_contract = ct;
                }
            }

            if (best_known >= 0 && best_contract) {
                /* Load the commodity for this contract (leave reserve for players) */
                commodity_t ingot = best_contract->commodity;
                npc->dest_station = best_contract->station_index;
                int take_units = contract_fit_manifest_count(best_contract,
                                                             &home->manifest);
                take_units -= hauler_reserve_units();
                if (take_units < 0) take_units = 0;
                int space_units = (int)floorf(space + 0.0001f);
                if (take_units > space_units) take_units = space_units;
                if (take_units > 0) {
                    if (hauler_ship) {
                        int moved = hauler_load_station_units_for_contract(
                            w, n, home, hauler_ship, best_contract, take_units);
                        if (moved > 0) {
                            station_finished_sync(home, ingot);
                            ship_finished_sync(hauler_ship, ingot);
                            hauler_sync_cargo_from_manifest(npc, hauler_ship);
                        }
                    } else {
                        float take = (float)take_units;
                        npc->cargo[ingot] += take;
                        home->_inventory_cache[ingot] -= take;
                        if (station_manifest_drain_commodity(home, ingot, take_units) > 0)
                            home->manifest_dirty = true;
                    }
                }
            }
            /* If no contract was fillable from known_contracts, the
             * hauler stays docked. Under the gossip-contract model the
             * hauler only acts on intel it has heard about via prior
             * dock contact — no peer-station scan, no surplus-push
             * stopgap. Idle haulers re-handshake at home each dock
             * cycle and pick up whatever new gossip arrives. */
            float total_carried = 0.0f;
            for (int c = 0; c < COMMODITY_COUNT; c++) total_carried += npc->cargo[c];
            if (total_carried < 0.01f) {
                /* No cargo loaded — stay docked at home and wait for
                 * stock or a contract. Do NOT migrate home_station to
                 * wherever surplus happens to be: an earlier version
                 * did that and every hauler in the world converged on
                 * the highest-stock station (Helios), permanently
                 * stalling the inter-station chain. Haulers belong to
                 * their spawn station; the auto-respawn loop replaces
                 * dead slots if a station's roster ever drops to zero. */
                npc->state_timer = HAULER_DOCK_TIME;
            } else {
                npc->state = NPC_STATE_TRAVEL_TO_DEST;
            }
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        station_t *dest = &w->stations[npc->dest_station];
        vec2 dock_lane = station_approach_target(dest, npc->ship.pos);
        vec2 approach = npc_target_routed_through_station_docks(w, npc, dock_lane);
        npc_steer_with_path(w, n, npc, approach, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        if (npc_reached_station_dock_lane(npc, dest, dock_lane)) {
            npc->ship.vel = v2(0.0f, 0.0f);
            npc->ship.pos = dock_lane;
            npc->state = NPC_STATE_UNLOADING;
            npc->state_timer = HAULER_LOAD_TIME;
        }
        break;
    }
    case NPC_STATE_UNLOADING: {
        npc->state_timer -= dt;
        npc->ship.vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            /* Dock-contact gossip with the destination station: ship
             * brings any contracts it picked up at home (or earlier
             * docks) and learns the dest's open demands. This is how
             * intel propagates one hop per round-trip. */
            gossip_dock_handshake(w, npc->dest_station,
                                  npc->known_contracts,
                                  &npc->known_contract_count,
                                  SHIP_KNOWN_CONTRACT_CAP,
                                  &npc->knowledge);

            station_t *dest = &w->stations[npc->dest_station];
            if (hauler_ship && hauler_ship->manifest.count > 0) {
                for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
                    commodity_t cargo = (commodity_t)i;
                    contract_t *ct = hauler_delivery_contract(
                        w, npc->dest_station, cargo, &hauler_ship->manifest);
                    if (!ct) continue;
                    int held = contract_fit_manifest_count(ct,
                                                           &hauler_ship->manifest);
                    if (held <= 0) continue;
                    int needed = (int)floorf(ct->quantity_needed + 0.0001f);
                    if (needed <= 0) continue;
                    int space_units = station_finished_room_units_for_hauler(
                        dest, cargo, MAX_PRODUCT_STOCK);
                    if (space_units <= 0) continue;
                    int request = held < space_units ? held : space_units;
                    if (request > needed) request = needed;
                    int moved = hauler_unload_ship_units_for_contract(
                        w, n, hauler_ship, dest, ct, request);
                    if (moved <= 0) continue;
                    station_finished_sync(dest, cargo);
                    ship_finished_sync(hauler_ship, cargo);
                    ct->quantity_needed -= (float)moved;
                    if (ct->quantity_needed < 0.0f) ct->quantity_needed = 0.0f;
                    float price = contract_price(ct);
                    if (price > 0.0f) {
                        ledger_earn_from_pool(dest, npc->session_token,
                                              price * (float)moved);
                    }
                }
                hauler_sync_cargo_from_manifest(npc, hauler_ship);
            } else {
                for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
                    if (npc->cargo[i] <= 0.0f) continue;
                    commodity_t cargo = (commodity_t)i;
                    contract_t *ct = hauler_delivery_contract(
                        w, npc->dest_station, cargo, NULL);
                    if (!ct) continue;
                    int held = (int)floorf(npc->cargo[i] + 0.0001f);
                    int needed = (int)floorf(ct->quantity_needed + 0.0001f);
                    int space_units = station_finished_room_units_for_hauler(
                        dest, cargo, MAX_PRODUCT_STOCK);
                    int delivered_units = held;
                    if (delivered_units > needed) delivered_units = needed;
                    if (delivered_units > space_units) delivered_units = space_units;
                    if (delivered_units <= 0) continue;
                    float delivered = (float)delivered_units;
                    float before = dest->_inventory_cache[i];
                    dest->_inventory_cache[i] += delivered;
                    if (dest->_inventory_cache[i] > MAX_PRODUCT_STOCK)
                        dest->_inventory_cache[i] = MAX_PRODUCT_STOCK;
                    /* Mirror the float bump into the manifest so the trade
                     * picker (manifest-only) sees the new stock. Use the
                     * post-clamp delta so overflow doesn't create phantom
                     * manifest entries. */
                    int int_delta = (int)floorf(dest->_inventory_cache[i] + 0.0001f)
                                  - (int)floorf(before + 0.0001f);
                    if (int_delta > 0) {
                        if (station_manifest_seed_from_npc(dest, (commodity_t)i,
                                                           int_delta, n) > 0)
                            dest->manifest_dirty = true;
                    }
                    ct->quantity_needed -= delivered;
                    if (ct->quantity_needed < 0.0f) ct->quantity_needed = 0.0f;
                    float price = contract_price(ct);
                    if (price > 0.0f && delivered > 0.01f) {
                        ledger_earn_from_pool(dest, npc->session_token,
                                               price * delivered);
                    }
                    npc->cargo[i] -= delivered;
                    if (npc->cargo[i] < 0.01f) npc->cargo[i] = 0.0f;
                }
            }
            /* Hauler also feeds delivered stock into scaffold station/modules. */
            if (dest->scaffold || dest->module_count > 0) {
                if (dest->scaffold) {
                    float needed_f = SCAFFOLD_MATERIAL_NEEDED * (1.0f - dest->scaffold_progress);
                    int held = station_finished_count(dest, COMMODITY_FRAME);
                    int needed = (int)ceilf(needed_f - 0.0001f);
                    if (needed < 0) needed = 0;
                    int request = held < needed ? held : needed;
                    int delivered = station_finished_drain(dest, COMMODITY_FRAME, request);
                    if (delivered > 0) {
                        dest->scaffold_progress += (float)delivered / SCAFFOLD_MATERIAL_NEEDED;
                        if (dest->scaffold_progress >= 1.0f)
                            activate_outpost(w, npc->dest_station);
                    }
                }
                /* Feed remaining station inventory into scaffolded modules. */
                ship_t module_feed_ship = {0};
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                    module_feed_ship.cargo[c] = dest->_inventory_cache[c];
                /* Hauler is paid via the contract path elsewhere; the
                 * build-material payout returned here is discarded. NPC
                 * economic identity tracking happens through
                 * ledger_credit_supply_amount on contract completion. */
                (void)step_module_delivery(w, dest, npc->dest_station,
                                           &module_feed_ship, COMMODITY_COUNT);
                /* Put remaining back. The float was drained into
                 * module_input by step_module_delivery; we have to drain
                 * the matching manifest entries too or the BUY picker
                 * (manifest-only) will keep advertising stock that the
                 * server-side float check (game_sim.c try_buy_product)
                 * sees as 0 and silently rejects. */
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                    float consumed = dest->_inventory_cache[c] - module_feed_ship.cargo[c];
                    if (consumed > 0.01f) {
                        dest->_inventory_cache[c] -= consumed;
                        int whole = (int)floorf(consumed + 0.0001f);
                        if (whole > 0) {
                            (void)station_manifest_consume_by_commodity(
                                dest, (commodity_t)c, whole);
                        }
                    }
                }
            }
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        vec2 dock_lane = station_approach_target(home, npc->ship.pos);
        vec2 approach_home = npc_target_routed_through_station_docks(w, npc, dock_lane);
        npc_steer_with_path(w, n, npc, approach_home, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        if (npc_reached_station_dock_lane(npc, home, dock_lane)) {
            npc->ship.vel = v2(0.0f, 0.0f);
            npc->ship.pos = dock_lane;
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            /* Dock auto-repair: NPC owes the home station for the
             * kits it consumes. Closed loop:
             *   1. NPC delivers a contract -> dest station credits its
             *      ledger from credit_pool (ledger_earn_from_pool).
             *   2. NPC docks at home with hull damage -> home applies
             *      kits up to (kits in stock, hull missing) and
             *      force-debits the NPC's ledger for the cost.
             *
             * Force-debit so a damaged hauler ALWAYS gets repaired
             * even if its balance is empty — the debt persists and
             * gets paid back as the hauler completes future contracts.
             * Otherwise a single bad scrape could permanently strand
             * a fresh drone with no income path. The home dock still
             * needs kits in stock; if not, no repair this cycle. */
            float max_h = npc_max_hull(npc);
            ship_t *ship = npc_ship_for(w, n);
            float cur_hull = ship ? ship->hull : npc->hull;
            if (cur_hull < max_h - 0.5f
                && station_has_module(home, MODULE_DOCK)) {
                int kits = station_finished_count(home, COMMODITY_REPAIR_KIT);
                int missing = (int)ceilf(max_h - cur_hull);
                int apply = kits < missing ? kits : missing;
                if (apply > 0) {
                    int drained = station_finished_drain(home, COMMODITY_REPAIR_KIT, apply);
                    if (drained > 0) {
                        apply = drained;
                        float kit_price = station_sell_price(home, COMMODITY_REPAIR_KIT);
                        if (kit_price < 0.01f) kit_price = 1.0f;
                        float cost = (float)apply * kit_price;
                        /* Force-debit -> balance can go negative, station
                         * still gets credited. Hauler pays it back over
                         * subsequent deliveries. */
                        ledger_force_debit(home, npc->session_token, cost, ship);
                        /* Write through ship layer; reverse-mirror at
                         * end of the NPC tick pushes the value back to
                         * npc->hull. */
                        if (ship) {
                            ship->hull += (float)apply;
                            if (ship->hull > max_h) ship->hull = max_h;
                        } else {
                            npc->hull += (float)apply;
                            if (npc->hull > max_h) npc->hull = max_h;
                        }
                    }
                }
            }
            /* Wear-and-tear maintenance: each home-dock visit consumes
             * one repair kit regardless of damage. This is the baseline
             * demand that keeps kits flowing through the economy --
             * without it, Prospect's kit shelf never drains (haulers
             * rarely take real damage), kit-import contracts never
             * trip, and the inter-station chain stalls. Force-debit so
             * the hauler is on the hook for upkeep just like a repair. */
            if (station_has_module(home, MODULE_DOCK)
                && station_finished_count(home, COMMODITY_REPAIR_KIT) >= 1) {
                if (station_finished_drain(home, COMMODITY_REPAIR_KIT, 1) <= 0)
                    break;
                float kit_price = station_sell_price(home, COMMODITY_REPAIR_KIT);
                if (kit_price < 0.01f) kit_price = 1.0f;
                ledger_force_debit(home, npc->session_token, kit_price, ship);
            }
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

/* Find an open ring slot at any active player outpost that
 * matches the given module type. Used by tow drones to pick a delivery
 * destination for a loose scaffold. Returns -1 if none. */
static int find_destination_for_scaffold(const world_t *w, module_type_t type,
                                        int exclude_station) {
    /* Pass 1: active outposts with a placement plan for this type.
     * Planned outposts are only valid for SIGNAL_RELAY below; otherwise
     * a hopper/furnace can accidentally become the founding scaffold. */
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            if (st->placement_plans[p].type == type) return s;
        }
    }
    /* Pass 2: any active outpost with at least one open ring slot. */
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
            if (ring > 1 && !ring_has_dock(st, ring - 1)) continue;
            if (station_ring_free_slot(st, ring, STATION_RING_SLOTS[ring]) >= 0)
                return s;
        }
    }
    /* Pass 3: SIGNAL_RELAY is special — it founds new outposts. If the
     * player has a planned (ghost) outpost waiting, deliver the relay
     * there even without an explicit placement plan, so the chicken-
     * and-egg of "first relay needs an outpost that needs a relay" is
     * resolved by the drone. */
    if (type == MODULE_SIGNAL_RELAY) {
        for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
            if (s == exclude_station) continue;
            const station_t *st = &w->stations[s];
            if (st->planned) return s;
        }
    }
    return -1;
}

/* Find a loose scaffold near this NPC's home station that has a known
 * destination. Returns scaffold index or -1. */
static int find_loose_scaffold_for_tow(const world_t *w, const npc_ship_t *npc) {
    const station_t *home = &w->stations[npc->home_station];
    const float pickup_range_sq = 4000.0f * 4000.0f;
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        if (sc->state != SCAFFOLD_LOOSE) continue;
        /* Skip scaffolds being towed by a player or another drone */
        if (sc->towed_by >= 0) continue;
        /* Must be near the home shipyard */
        float d_home = v2_dist_sq(sc->pos, home->pos);
        if (d_home > pickup_range_sq) continue;
        /* Must have a place to deliver (not back to home station) */
        if (find_destination_for_scaffold(w, sc->module_type, npc->home_station) < 0) continue;
        if (d_home < best_d) { best_d = d_home; best = i; }
    }
    return best;
}

/* Tow drone: spawned at shipyards, picks up loose scaffolds, delivers
 * them to player outposts with placement plans, returns home. Reuses
 * the existing NPC state enum but interprets the states for tow logic.
 *
 *   DOCKED → look for a loose scaffold + matching destination
 *   TRAVEL_TO_ASTEROID → fly to scaffold position (ASTEROID = "thing to grab")
 *   MINING → grab phase: tractor it, set towed_scaffold
 *   TRAVEL_TO_DEST → tow it to destination outpost
 *   UNLOADING → release near open slot, let it snap
 *   RETURN_TO_STATION → fly back to home shipyard
 */
static void step_tow_drone(world_t *w, npc_ship_t *npc, int n, float dt) {
    /* If we lost our towed scaffold mid-flight (destroyed, snapped early,
     * picked up by a player), drop back to idle. */
    if (npc->towed_scaffold >= 0) {
        scaffold_t *sc = &w->scaffolds[npc->towed_scaffold];
        if (!sc->active || sc->state == SCAFFOLD_PLACED ||
            sc->state == SCAFFOLD_SNAPPING || sc->towed_by != -2 - n) {
            npc->towed_scaffold = -1;
            if (npc->state == NPC_STATE_TRAVEL_TO_DEST ||
                npc->state == NPC_STATE_UNLOADING) {
                npc->state = NPC_STATE_RETURN_TO_STATION;
            }
        }
    }

    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->ship.vel = v2(0.0f, 0.0f);
        if (npc->state_timer > 0.0f) break;
        int sc_idx = find_loose_scaffold_for_tow(w, npc);
        if (sc_idx < 0) {
            npc->state_timer = 2.0f; /* nothing to tow, idle and recheck */
            break;
        }
        npc->target_asteroid = sc_idx;  /* repurpose: scaffold idx for tow */
        npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
        break;
    }
    case NPC_STATE_TRAVEL_TO_ASTEROID: {
        if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_SCAFFOLDS) {
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
        }
        scaffold_t *sc = &w->scaffolds[npc->target_asteroid];
        if (!sc->active || sc->state != SCAFFOLD_LOOSE || sc->towed_by >= 0) {
            npc->target_asteroid = -1;
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
        }
        vec2 sc_target = npc_target_routed_through_station_docks(w, npc, sc->pos);
        npc_steer_with_path(w, n, npc, sc_target, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        if (v2_dist_sq(npc->ship.pos, sc->pos) < 80.0f * 80.0f) {
            /* Grab — claim the scaffold and switch to tow mode.
             * Use towed_by = -2 - drone_index so positive values keep
             * meaning "player id" and negative values < -1 mean "drone n". */
            sc->towed_by = -2 - n;
            sc->state = SCAFFOLD_TOWING;
            npc->towed_scaffold = npc->target_asteroid;
            int dest = find_destination_for_scaffold(w, sc->module_type, npc->home_station);
            if (dest < 0) {
                /* Destination vanished while we were en route; drop and reset */
                sc->towed_by = -1;
                sc->state = SCAFFOLD_LOOSE;
                npc->towed_scaffold = -1;
                npc->target_asteroid = -1;
                npc->state = NPC_STATE_DOCKED;
                npc->state_timer = HAULER_DOCK_TIME;
                break;
            }
            npc->dest_station = dest;
            npc->state = NPC_STATE_TRAVEL_TO_DEST;
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        if (npc->towed_scaffold < 0 ||
            npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS) {
            npc->state = NPC_STATE_RETURN_TO_STATION;
            break;
        }
        scaffold_t *sc = &w->scaffolds[npc->towed_scaffold];
        station_t *dest = &w->stations[npc->dest_station];
        /* Spring-chase the scaffold behind the drone. Pull-only spring at
         * rest_length=60; 1D axial damping along the rope; small tangent
         * drag to bleed orbital drift without locking lateral motion. */
        static const tractor_beam_t SCAFFOLD_TOW = {
            .rest_length     = 60.0f,
            .pull_strength   = 8.0f,
            .push_strength   = 0.0f,
            .range           = 0.0f,
            .axial_damping   = 0.6f,
            .tangent_damping = 0.15f,   /* ~25% of axial — anti-orbit, tunable */
            .speed_cap       = 0.0f,
            .falloff         = TRACTOR_FALLOFF_CONSTANT,
        };
        tractor_anchor_t src_drone = { .pos = npc->ship.pos, .vel = NULL,     .inv_mass = 0.0f };
        tractor_anchor_t tgt_sc    = { .pos = sc->pos,       .vel = &sc->vel, .inv_mass = 1.0f };
        (void)tractor_apply(&src_drone, &tgt_sc, &SCAFFOLD_TOW, dt);
        sc->pos = v2_add(sc->pos, v2_scale(sc->vel, dt));

        vec2 approach = station_approach_target(dest, npc->ship.pos);
        npc_steer_with_path(w, n, npc, approach, /*thrust_scale=*/0.6f, dt);
        /* Speed cap while towing — heavy load */
        float spd = v2_len(npc->ship.vel);
        if (spd > 60.0f) npc->ship.vel = v2_scale(npc->ship.vel, 60.0f / spd);
        npc_apply_physics(npc, dt, w);
        float scaffold_arrival = dest->planned ? 700.0f : 600.0f;
        float sc_d = v2_dist_sq(sc->pos, dest->pos);
        if (sc_d < scaffold_arrival * scaffold_arrival) {
            /* Release — let the existing snap-to-slot logic in step_scaffolds
             * pick up the loose scaffold near the outpost ring. */
            sc->towed_by = -1;
            sc->state = SCAFFOLD_LOOSE;
            npc->towed_scaffold = -1;
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        vec2 dock_lane = station_approach_target(home, npc->ship.pos);
        vec2 approach = npc_target_routed_through_station_docks(w, npc, dock_lane);
        npc_steer_with_path(w, n, npc, approach, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        if (npc_reached_station_dock_lane(npc, home, dock_lane)) {
            npc->ship.vel = v2(0.0f, 0.0f);
            npc->ship.pos = dock_lane;
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

/* Cooldown between auto-respawn attempts. 15 s feels recoverable
 * (full chain-wipe of 7 NPCs comes back over ~100 s) without making
 * the rocks-vs-NPC PvP feature feel toothless. */
#define NPC_RESPAWN_INTERVAL 15.0f

void step_npc_ships(world_t *w, float dt) {
    /* Replenish dead haulers/miners on a slow drip. The first call
     * after world_reset waits the full interval so the seeded roster
     * isn't immediately doubled. */
    if (w->npc_respawn_timer <= 0.0f) w->npc_respawn_timer = NPC_RESPAWN_INTERVAL;
    w->npc_respawn_timer -= dt;
    if (w->npc_respawn_timer <= 0.0f) {
        w->npc_respawn_timer = NPC_RESPAWN_INTERVAL;
        (void)replenish_npc_roster(w);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* Despawn-on-destroy: the spawn loop replaces dead slots on
         * the next tick. Cargo is lost (the chain takes a hit when a
         * loaded hauler dies — that's the cost of letting them get
         * smashed by asteroids). Read ship.hull (authoritative since
         * Slice 9-11) so external damage delivered between ticks via
         * apply_npc_ship_damage despawns immediately rather than
         * limping one extra tick. */
        const ship_t *paired_ship = npc_ship_for(w, n);
        float live_hull = paired_ship ? paired_ship->hull : npc->hull;
        if (live_hull <= 0.0f) {
            SIM_LOG("[npc] %d (role=%d) destroyed — hull 0\n", n, (int)npc->role);
            npc->active = false;
            character_free_for_npc(w, n);
            continue;
        }
        npc->thrusting = false;
        npc->ship.tractor_active = false;
        /* Slice 13: pull external ship.pos/vel/angle writes into the
         * npc fields before physics integration this tick. */
        mirror_ship_pos_to_npc(w, n);
        npc_enforce_role_hull(npc);
        mirror_npc_to_character(w, n);
        npc_validate_stations(w, npc);

        /* Neural brain control: when brain_mode is set, the brain handles
         * flight. Physics + collision are still applied by the caller. */
        if (npc->brain_mode == 1 && signal_brain_loaded()) {
            signal_brain_drive_npc(w, npc, dt);
            /* Brain sets turn/thrust — let physics integration do the rest.
             * Skip the role-based state machine below. */
            mirror_ship_to_npc(w, n);
            continue;
        }

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            mirror_ship_to_npc(w, n);
            continue;
        }
        if (npc->role == NPC_ROLE_TOW) {
            step_tow_drone(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            mirror_ship_to_npc(w, n);
            continue;
        }

        const hull_def_t *hull = npc_hull_def(npc);
        switch (npc->state) {
        case NPC_STATE_DOCKED: {
            npc->state_timer -= dt;
            npc->ship.vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
                /* Prefer towing a loose fragment over fracturing fresh
                 * rock, but only if the fragment is inside this ship's
                 * actual tractor range. NPC miners do not get a remote
                 * claim/pull channel. */
                if (npc_try_claim_loose_fragment(w, npc, 0.0f)) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                /* No useful ore demand? Don't add more mass — IDLE and
                 * wait for fragments to drift through, or for the local
                 * chain to drain back into a useful state. */
                if (npc_home_has_no_ore_need(w, npc)) {
                    npc->state = NPC_STATE_IDLE;
                    npc->state_timer = 5.0f;
                    break;
                }
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) {
                    npc->target_asteroid = target;
                    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                } else {
                    npc->state = NPC_STATE_IDLE;
                    npc->state_timer = 2.0f;
                }
            }
            break;
        }
        case NPC_STATE_TRAVEL_TO_ASTEROID: {
            if (!npc_target_valid(w, npc)) {
                /* Same fragment-first rule when the current target dies
                 * (someone else fractured it, etc.). */
                if (npc_try_claim_loose_fragment(w, npc, 0.0f)) {
                    npc->target_asteroid = -1;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                if (npc_home_has_no_ore_need(w, npc)) {
                    npc->target_asteroid = -1;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) npc->target_asteroid = target;
                else { npc->target_asteroid = -1; npc->state = NPC_STATE_RETURN_TO_STATION; break; }
            }
            /* Pre-empt for FRACTURE distress: a stuck hauler may have
             * posted a distress contract since we left dock. Switch
             * only if the distress target is SIGNIFICANTLY closer than
             * our current target (≥50% nearer) — keeps the response
             * fast for genuine shortcut detours but prevents thrashing
             * mid-approach when the current target is close enough that
             * any FRACTURE rock looks "closer" each tick. */
            if (npc->towed_fragment < 0 && npc_target_valid(w, npc)) {
                vec2 cur_pos = w->asteroids[npc->target_asteroid].pos;
                float cur_d2 = v2_dist_sq(npc->ship.pos, cur_pos);
                const float MAX_DISTRESS_PREEMPT_SQ = 2500.0f * 2500.0f;
                for (int k = 0; k < MAX_CONTRACTS; k++) {
                    if (!w->contracts[k].active) continue;
                    if (w->contracts[k].action != CONTRACT_FRACTURE) continue;
                    int idx = w->contracts[k].target_index;
                    if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
                    if (w->asteroids[idx].tier < max_mineable_tier(npc->ship.mining_level)) continue;
                    if (idx == npc->target_asteroid) break;
                    float new_d2 = v2_dist_sq(npc->ship.pos, w->asteroids[idx].pos);
                    if (new_d2 > MAX_DISTRESS_PREEMPT_SQ) continue;
                    if (new_d2 < cur_d2 * 0.25f) { npc->target_asteroid = idx; break; }
                }
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            vec2 ast_target = npc_target_routed_through_station_docks(w, npc, a->pos);
            npc_steer_with_path(w, n, npc, ast_target, /*thrust_scale=*/1.0f, dt);
            npc_apply_physics(npc, dt, w);
            if (v2_dist_sq(npc->ship.pos, a->pos) < MINING_RANGE * MINING_RANGE)
                npc->state = NPC_STATE_MINING;
            break;
        }
        case NPC_STATE_MINING: {
            if (!npc_target_valid(w, npc)) {
                /* Target gone — look for a fragment to tow, or find new target */
                if (npc->towed_fragment >= 0) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else if (npc_try_claim_loose_fragment(w, npc, 0.0f)) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else if (npc_home_has_no_ore_need(w, npc)) {
                    /* Hopper full and no fragment in range — head home
                     * and IDLE there until the smelter drains. */
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else {
                    int target = npc_find_mineable_asteroid(w, npc);
                    if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                    else npc->state = NPC_STATE_RETURN_TO_STATION;
                }
                break;
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            float dist_sq = v2_dist_sq(npc->ship.pos, a->pos);
            float standoff = a->radius + 60.0f;
            float approach_r = standoff + 20.0f;

            /* If we got shoved past mining range entirely (NPC↔NPC
             * collision, fracture knockback, gravity), drop back to
             * TRAVEL so the renderer doesn't keep drawing a beam to a
             * target we can't actually fire at. The MINING state is
             * for ships in firing position, not for "approaching from
             * across the map". */
            if (dist_sq > MINING_RANGE * MINING_RANGE) {
                npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                break;
            }

            if (dist_sq > approach_r * approach_r) {
                npc_steer_toward(npc, a->pos, dt);
                npc_apply_physics(npc, dt, w);
                break;
            }

            /* Hover near the rock via flight controller. The away-push
             * and extra damping below are role-specific overlays that
             * sit on top of shared sim_ship physics — same pattern as
             * player tow drag in game_sim.c. They're intentional, not a
             * leftover from the unification: hover wants different
             * brake semantics than flight_steer_to (push radially away
             * from the target, not reverse along velocity), and the 4.0
             * extra damping is what holds the standoff distance. */
            {
                flight_cmd_t cmd = flight_hover_near(w, &npc->ship, a->pos, standoff);
                if (cmd.thrust < 0.0f) {
                    vec2 away = v2_norm(v2_sub(npc->ship.pos, a->pos));
                    npc->ship.vel = v2_add(npc->ship.vel, v2_scale(away, hull->accel * 0.5f * dt));
                    cmd.thrust = 0.0f;
                }
                npc_apply_flight_cmd(npc, cmd, dt);
                /* Hover never lights the engine flame — keep prior visual. */
                npc->thrusting = false;
            }
            npc->ship.vel = v2_scale(npc->ship.vel, 1.0f / (1.0f + (4.0f * dt)));
            npc_apply_physics(npc, dt, w);

            /* Strict range+cone gate before firing — same metric the player
             * uses. Without this, NPCs that get shoved (NPC↔NPC collision,
             * gravity, fracture knockback) used to keep the MINING state
             * and beam-render across the map. If we lost the firing line,
             * fall back to TRAVEL so steering pulls us back into range. */
            vec2 forward = v2_from_angle(npc->ship.angle);
            vec2 muzzle = ship_muzzle(npc->ship.pos, npc->ship.angle, &npc->ship);
            int mining_level = npc->ship.mining_level;
            float sig_eff = signal_mining_efficiency(signal_strength_at(w, npc->ship.pos));
            mining_beam_t mb = sim_mining_beam_step(w, muzzle, forward,
                npc->target_asteroid, mining_level,
                ship_mining_rate(&npc->ship), sig_eff, /*fracturer*/ -1, dt);

            if (!mb.hit) {
                npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                break;
            }

            if (!mb.fired && !mb.fractured) {
                break;
            }

            if (mb.fractured) {
                npc->target_asteroid = -1;

                /* Grab the nearest S-tier fragment to tow home */
                float tractor_r = ship_tractor_range(&npc->ship);
                float best_frag_d = tractor_r * tractor_r;
                int best_frag = -1;
                if (tractor_r > 0.0f) {
                    for (int fi = 0; fi < MAX_ASTEROIDS; fi++) {
                        asteroid_t *f = &w->asteroids[fi];
                        if (!f->active || f->tier != ASTEROID_TIER_S) continue;
                        if (!npc_home_has_smelt_endpoint(w, npc, f->commodity, NULL)) continue;
                        float fd = v2_dist_sq(npc->ship.pos, f->pos);
                        if (fd < best_frag_d) { best_frag_d = fd; best_frag = fi; }
                    }
                }
                if (best_frag >= 0) {
                    npc->towed_fragment = best_frag;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    /* Stamp the NPC's token onto the towed fragment so
                     * the eventual smelt-payout credits the NPC's ledger.
                     * Skip if a non-NPC token is already there — the
                     * player who first towed this fragment keeps the
                     * payout claim. */
                    asteroid_t *frag = &w->asteroids[best_frag];
                    bool stamped = false;
                    for (int b = 0; b < 8 && !stamped; b++)
                        if (frag->last_towed_token[b]) stamped = true;
                    if (!stamped || token_is_npc(frag->last_towed_token)) {
                        memcpy(frag->last_towed_token, npc->session_token,
                               sizeof(frag->last_towed_token));
                    }
                }
            }
            break;
        }
        case NPC_STATE_RETURN_TO_STATION: {
            station_t *home = &w->stations[npc->home_station];

            /* Deliver to the same furnace+ore-hopper midpoint the smelter
             * beam uses. Picking the first furnace strands crystal at
             * Helios when the first furnace is cuprite-tagged. */
            vec2 delivery_target = home->pos;
            bool have_delivery_target = false;
            if (npc->towed_fragment >= 0 && npc->towed_fragment < MAX_ASTEROIDS) {
                asteroid_t *tow = &w->asteroids[npc->towed_fragment];
                if (tow->active) {
                    have_delivery_target = station_smelt_pair_for_fragment(home,
                                                                           npc->home_station,
                                                                           tow,
                                                                           &delivery_target);
                    if (!have_delivery_target) {
                        npc->towed_fragment = -1;
                        npc->state = NPC_STATE_IDLE;
                        npc->state_timer = 2.0f;
                        break;
                    }
                }
            }
            if (!have_delivery_target) {
                delivery_target = station_approach_target(home, npc->ship.pos);
            }

            /* Slow down when towing so the fragment can keep up */
            float tow_thrust_scale = (npc->towed_fragment >= 0) ? 0.5f : 1.0f;
            delivery_target = npc_target_routed_through_station_docks(w, npc, delivery_target);
            npc_steer_with_path(w, n, npc, delivery_target, tow_thrust_scale, dt);
            npc_apply_physics(npc, dt, w);

            /* Speed cap when towing */
            if (npc->towed_fragment >= 0) {
                npc->ship.tractor_active = true;
                float spd = v2_len(npc->ship.vel);
                float max_tow_speed = 80.0f;
                if (spd > max_tow_speed)
                    npc->ship.vel = v2_scale(npc->ship.vel, max_tow_speed / spd);
            }

            /* Tow the fragment with the same elastic band used by player
             * ships. NPC miners must keep the rock inside their actual
             * tractor envelope; if they outrun it, the band snaps. */
            if (npc->towed_fragment >= 0 && npc->towed_fragment < MAX_ASTEROIDS) {
                asteroid_t *tow = &w->asteroids[npc->towed_fragment];
                if (tow->active) {
                    float tractor_r = ship_tractor_range(&npc->ship);
                    float dist = v2_len(v2_sub(npc->ship.pos, tow->pos));
                    if (tractor_r <= 0.0f || dist > tractor_r * 1.5f) {
                        npc->towed_fragment = -1;
                        break;
                    }
                    ship_apply_fragment_tow(&npc->ship, tow, dt);
                    /* Release when close to the furnace — let the furnace tractor take over */
                    float furnace_d = v2_dist_sq(tow->pos, delivery_target);
                    if (furnace_d < 150.0f * 150.0f) {
                        npc->towed_fragment = -1;
                    }
                } else {
                    npc->towed_fragment = -1;
                }
            } else if (npc->towed_fragment >= MAX_ASTEROIDS) {
                npc->towed_fragment = -1;
            }

            /* Once fragment is delivered (or lost), go find more ore */
            if (npc->towed_fragment < 0) {
                /* Drift away from the furnace, then look for next target */
                npc->state = NPC_STATE_IDLE;
                npc->state_timer = 2.0f;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_IDLE: {
            npc_apply_physics(npc, dt, w);
            npc->state_timer -= dt;
            if (npc->state_timer <= 0.0f) {
                /* IDLE → fragment first, fracture second. */
                if (npc_try_claim_loose_fragment(w, npc, 0.0f)) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                /* No useful ore demand → stay idle until either a
                 * fragment drifts into range or the local chain drains. */
                if (npc_home_has_no_ore_need(w, npc)) {
                    npc->state_timer = 5.0f;
                    break;
                }
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                else npc->state_timer = 3.0f;
            }
            break;
        }
        default: break;
        }

        /* Re-mirror after the dispatch wrote npc->target_asteroid /
         * state / etc., so the next miner processed in the same tick
         * sees fresh target contention via characters[]. */
        mirror_npc_to_character(w, n);

        /* NPC collision with stations and asteroids */
        if (npc->state != NPC_STATE_DOCKED) {
            npc_resolve_station_collisions(w, npc);
            npc_resolve_asteroid_collisions(w, npc);
        }
        /* Reverse-mirror ship -> npc after damage was applied through
         * the ship layer (#294 Slice 9). Keeps npc->hull authoritative
         * for the despawn check at the top of the next tick. */
        mirror_ship_to_npc(w, n);

        npc_update_manifest_rarity_tint(npc, npc_ship_for(w, n), dt);
    }
}

/* Generate DESTROY contracts for asteroids blocking stuck NPCs. */
void generate_npc_distress_contracts(world_t *w, float dt) {
    const float STUCK_CONTRACT_DELAY = 6.0f;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* Only haulers in transit can get stuck */
        if (npc->role != NPC_ROLE_HAULER) continue;
        if (npc->state != NPC_STATE_TRAVEL_TO_DEST && npc->state != NPC_STATE_RETURN_TO_STATION) continue;
        int route_station = (npc->state == NPC_STATE_TRAVEL_TO_DEST)
            ? npc->dest_station
            : npc->home_station;
        if (route_station >= 0 && route_station < MAX_STATIONS &&
            station_is_active(&w->stations[route_station]) &&
            npc_point_inside_station_nav_envelope(&w->stations[route_station], npc->ship.pos)) {
            npc->state_timer = 0.0f;
            continue;
        }
        /* Check if stuck: low speed away from stations for a while.
         * state_timer is unused by TRAVEL_TO_DEST / RETURN_TO_STATION,
         * so it doubles as a small sustained-stall timer here. */
        float speed = v2_len(npc->ship.vel);
        if (speed > 15.0f) {
            npc->state_timer = 0.0f;
            continue;
        }
        npc->state_timer += dt;
        if (npc->state_timer < STUCK_CONTRACT_DELAY) continue;
        /* Find nearest blocking asteroid */
        int blocker = -1;
        float best_d = 200.0f * 200.0f; /* within 200u */
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (!w->asteroids[i].active || asteroid_is_collectible(&w->asteroids[i])) continue;
            float d = v2_dist_sq(npc->ship.pos, w->asteroids[i].pos);
            if (d < best_d) { best_d = d; blocker = i; }
        }
        if (blocker < 0) continue;
        /* Check if a FRACTURE contract already exists for this asteroid. */
        bool exists = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (w->contracts[k].active &&
                w->contracts[k].action == CONTRACT_FRACTURE &&
                w->contracts[k].target_index == blocker &&
                contract_asteroid_target_matches(&w->contracts[k],
                                                 &w->asteroids[blocker])) {
                exists = true; break;
            }
        }
        if (exists) continue;
        /* Post distress contract */
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active) {
                w->contracts[k] = (contract_t){
                    .active = true, .action = CONTRACT_FRACTURE,
                    .station_index = (uint8_t)npc->home_station,
                    .target_pos = w->asteroids[blocker].pos,
                    .target_index = blocker,
                    .base_price = 20.0f, .age = 0.0f,
                    .claimed_by = -1,
                };
                contract_set_target_pub_from_asteroid(&w->contracts[k],
                                                      &w->asteroids[blocker]);
                break;
            }
        }
    }
}
