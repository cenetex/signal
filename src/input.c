/*
 * input.c -- Input handling for the Signal Space Miner client.
 */
#include <stdarg.h>
#include "input.h"
#include "local_server.h"
#include "music.h"
#include "net.h"
#include "onboarding.h"
#include "signal_model.h"

void clear_input_state(void) {
    memset(g.input.key_down, 0, sizeof(g.input.key_down));
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
}

void consume_pressed_input(void) {
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
}

void set_notice(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g.notice, sizeof(g.notice), fmt, args);
    va_end(args);
    g.notice_timer = 3.0f;
}

bool is_key_down(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_down[key];
}

bool is_key_pressed(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_pressed[key];
}

/* Compute which rings are unlocked on a station.
 * Ring 1 is always available.
 * Ring 2 unlocks when ring 1 has 2+ committed entries (modules + plans).
 * Ring 3 unlocks when ring 2 has 4+ committed entries. */
static int station_max_unlocked_ring(const station_t *st) {
    int counts[STATION_NUM_RINGS + 1] = {0};
    for (int m = 0; m < st->module_count; m++) {
        int r = st->modules[m].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS) counts[r]++;
    }
    for (int p = 0; p < st->placement_plan_count; p++) {
        int r = st->placement_plans[p].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS) counts[r]++;
    }
    int unlocked = 1;
    if (counts[1] >= 2) unlocked = 2;
    if (counts[2] >= 4) unlocked = 3;
    return unlocked;
}

/* Build a flat list of (station, ring, slot) tuples for every open slot
 * across all player outposts in snap range of a position. Returns the
 * count. Sorted so the slot whose world position is closest to `pos`
 * comes first — that becomes the default reticle target. */
typedef struct {
    int station;
    int ring;
    int slot;
    float dist_sq; /* sort key */
} reticle_target_t;
#define RETICLE_MAX_TARGETS 32

static int collect_reticle_targets(vec2 pos, reticle_target_t *out, int max) {
    int count = 0;
    const float SNAP_RANGE_SQ = 600.0f * 600.0f;
    for (int s = 3; s < MAX_STATIONS && count < max; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st) || st->scaffold) continue;
        /* Include planned stations — they accept plans even though they
         * have no physical presence yet. */
        if (v2_dist_sq(st->pos, pos) > SNAP_RANGE_SQ) continue;
        int max_ring = station_max_unlocked_ring(st);
        for (int ring = 1; ring <= max_ring && count < max; ring++) {
            int slots = STATION_RING_SLOTS[ring];
            for (int slot = 0; slot < slots && count < max; slot++) {
                bool taken = false;
                for (int m = 0; m < st->module_count; m++)
                    if (st->modules[m].ring == ring && st->modules[m].slot == slot) {
                        taken = true; break;
                    }
                if (taken) continue;
                vec2 sp = module_world_pos_ring(st, ring, slot);
                out[count].station = s;
                out[count].ring = ring;
                out[count].slot = slot;
                out[count].dist_sq = v2_dist_sq(sp, pos);
                count++;
            }
        }
    }
    /* Sort by distance ascending (insertion sort, count is small) */
    for (int i = 1; i < count; i++) {
        reticle_target_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].dist_sq > key.dist_sq) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

input_intent_t sample_input_intent(void) {
    input_intent_t intent = { 0 };
    intent.place_target_station = -1;
    intent.place_target_ring = -1;
    intent.place_target_slot = -1;
    intent.plan_station = -1;
    intent.plan_ring = -1;
    intent.plan_slot = -1;
    intent.cancel_planned_station = -1;
    intent.service_sell_only = COMMODITY_COUNT; /* default: deliver all */

    if (is_key_down(SAPP_KEYCODE_A) || is_key_down(SAPP_KEYCODE_LEFT)) {
        intent.turn += 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_D) || is_key_down(SAPP_KEYCODE_RIGHT)) {
        intent.turn -= 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_W) || is_key_down(SAPP_KEYCODE_UP)) {
        intent.thrust += 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_S) || is_key_down(SAPP_KEYCODE_DOWN)) {
        intent.thrust -= 1.0f;
    }

    if (intent.thrust != 0.0f || intent.turn != 0.0f)
        onboarding_mark_moved();

    intent.mine = is_key_down(SAPP_KEYCODE_R);

    /* Tractor: hold Space = grab, tap Space = release.
     * Track press time; on release, if held < 200ms = tap (release). */
    if (is_key_down(SAPP_KEYCODE_SPACE) && !g.plan_mode_active) {
        if (g.input.tractor_press_time == 0.0f)
            g.input.tractor_press_time = g.world.time;
        intent.tractor_hold = true;
    } else {
        if (g.input.tractor_press_time > 0.0f) {
            float held = g.world.time - g.input.tractor_press_time;
            if (held < 0.2f)
                intent.release_tow = true;  /* tap = release */
            g.input.tractor_press_time = 0.0f;
        }
    }
    intent.reset = is_key_pressed(SAPP_KEYCODE_X) && !LOCAL_PLAYER.docked;
    /* Safety: clear placement reticle if no longer towing or now docked */
    if (g.placement_reticle_active &&
        (LOCAL_PLAYER.docked || LOCAL_PLAYER.ship.towed_scaffold < 0)) {
        g.placement_reticle_active = false;
    }
    /* Close inspect pane when docked or thrusting */
    if (LOCAL_PLAYER.docked) { g.inspect_station = -1; g.inspect_module = -1; }
    /* SPACE (laser) auto-targets nearest module in beam cone */
    if (intent.mine && !LOCAL_PLAYER.docked &&
        LOCAL_PLAYER.in_dock_range && LOCAL_PLAYER.nearby_station >= 0) {
        const station_t *st = &g.world.stations[LOCAL_PLAYER.nearby_station];
        vec2 fwd = v2_from_angle(LOCAL_PLAYER.ship.angle);
        float tr = ship_tractor_range(&LOCAL_PLAYER.ship);
        float tr_sq = tr * tr;
        float best_dot = -1.0f;
        int best_mod = -1;
        for (int idx = 0; idx < st->module_count; idx++) {
            if (st->modules[idx].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[idx].ring, st->modules[idx].slot);
            if (v2_dist_sq(LOCAL_PLAYER.ship.pos, mp) > tr_sq) continue;
            vec2 to_mod = v2_sub(mp, LOCAL_PLAYER.ship.pos);
            float len = v2_len(to_mod);
            if (len < 1.0f) continue;
            float d = v2_dot(fwd, v2_scale(to_mod, 1.0f / len));
            if (d > 0.7f && d > best_dot) {
                best_dot = d;
                best_mod = idx;
            }
        }
        if (best_mod >= 0) {
            g.target_station = LOCAL_PLAYER.nearby_station;
            g.target_module = best_mod;
        } else {
            g.target_station = -1;
            g.target_module = -1;
        }
    }
    /* Clear target if laser released or out of range */
    if (!intent.mine) {
        /* Keep target briefly so E can fire it, but clear if moved away */
        if (g.target_station >= 0 && g.target_module >= 0) {
            const station_t *tst = &g.world.stations[g.target_station];
            if (g.target_module < tst->module_count) {
                vec2 mp = module_world_pos_ring(tst, tst->modules[g.target_module].ring,
                                                 tst->modules[g.target_module].slot);
                float tr = ship_tractor_range(&LOCAL_PLAYER.ship);
                if (v2_dist_sq(LOCAL_PLAYER.ship.pos, mp) > tr * tr * 1.5f) {
                    g.target_station = -1;
                    g.target_module = -1;
                }
            }
        }
    }
    /* E key: activate targeted module, or dock/launch if no target.
     * Special case: while docked on the CONTRACTS tab, [E] is the
     * "deliver" key (handled in the per-tab block below), not launch. */
    bool e_handled_by_contracts_tab =
        LOCAL_PLAYER.docked && g.station_tab == STATION_TAB_CONTRACTS;
    if (is_key_pressed(SAPP_KEYCODE_E) && !e_handled_by_contracts_tab) {
        if (LOCAL_PLAYER.docked) {
            /* Launch */
            intent.interact = true;
        } else if (g.target_station >= 0 && g.target_module >= 0) {
            /* E on targeted module: dock if it's a dock, otherwise inspect */
            const station_t *tst = &g.world.stations[g.target_station];
            if (g.target_module < tst->module_count) {
                if (tst->modules[g.target_module].type == MODULE_DOCK) {
                    intent.interact = true;
                } else {
                    /* Toggle module info pane */
                    if (g.inspect_station == g.target_station && g.inspect_module == g.target_module) {
                        g.inspect_station = -1;
                        g.inspect_module = -1;
                    } else {
                        g.inspect_station = g.target_station;
                        g.inspect_module = g.target_module;
                    }
                }
            }
            g.target_station = -1;
            g.target_module = -1;
        } else if (LOCAL_PLAYER.in_dock_range) {
            /* No target but near station — dock */
            intent.interact = true;
        }
    }

    /* Number keys: context-dependent */
    if (LOCAL_PLAYER.docked && g.station_tab == STATION_TAB_SHIPYARD) {
        const station_t *st = current_station_ptr();
        /* Shipyard tab: 1-9 order a scaffold.
         * Surface every unlocked module type this yard can fabricate.
         * (Plans are still useful for slot reservation in plan mode, but
         * are no longer required to *order* a kit — the chicken-and-egg
         * for the very first SIGNAL_RELAY would be unsolvable otherwise.) */
        int shown = 0;
        for (int t = 0; t < MODULE_COUNT && shown < 9; t++) {
            module_type_t kit = (module_type_t)t;
            if (module_kind(kit) == MODULE_KIND_NONE) continue;
            if (!station_has_module(st, kit)) continue;
            if (!module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules, kit)) continue;
            if (is_key_pressed(SAPP_KEYCODE_1 + shown)) {
                if (st->pending_scaffold_count >= 4) {
                    set_notice("Shipyard queue full.");
                } else if ((int)lroundf(LOCAL_PLAYER.ship.credits) < scaffold_order_fee(kit)) {
                    set_notice("Need %d cr to order.", scaffold_order_fee(kit));
                } else {
                    intent.buy_scaffold_kit = true;
                    intent.scaffold_kit_module = kit;
                    set_notice("Ordered %s scaffold.", module_type_name(kit));
                }
                break;
            }
            shown++;
        }
    } else if (LOCAL_PLAYER.docked && g.station_tab == STATION_TAB_CONTRACTS) {
        /* Contracts tab keys:
         *   [1/2/3] select a contract slot for selective delivery
         *   [E]     deliver — selective if a slot is selected, else all
         *
         * The display in station_ui.c sorts deliverable contracts first
         * so [1] usually picks "the contract you can fulfill right now".
         * Selecting via [1/2/3] also tracks that contract for the HUD.
         *
         * The selection persists until [E] consumes it or the player
         * switches tabs. Player walks away with their iridium intact
         * by selecting the ferrite contract before pressing E. */
        const station_t *here_st = current_station_ptr();
        int here_idx = LOCAL_PLAYER.current_station;

        /* Re-derive the same slot ordering as station_ui.c so the
         * keypress maps to the visible row. */
        int slot_contract[3] = {-1, -1, -1};
        int slot_count = 0;
        for (int ci = 0; ci < MAX_CONTRACTS && slot_count < 3; ci++) {
            const contract_t *ct = &g.world.contracts[ci];
            if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
            if (here_idx < 0 || ct->station_index != here_idx) continue;
            if (LOCAL_PLAYER.ship.cargo[ct->commodity] < 0.5f) continue;
            slot_contract[slot_count++] = ci;
        }
        if (slot_count < 3 && here_st) {
            int nearest[3] = {-1, -1, -1};
            float nearest_d[3] = {1e18f, 1e18f, 1e18f};
            vec2 here = here_st->pos;
            for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
                const contract_t *ct = &g.world.contracts[ci];
                if (!ct->active || ct->station_index >= MAX_STATIONS) continue;
                if (!station_exists(&g.world.stations[ct->station_index])) continue;
                bool already = false;
                for (int s = 0; s < slot_count; s++)
                    if (slot_contract[s] == ci) { already = true; break; }
                if (already) continue;
                vec2 target = (ct->action == CONTRACT_TRACTOR) ? g.world.stations[ct->station_index].pos : ct->target_pos;
                float d = v2_dist_sq(here, target);
                for (int s = 0; s < 3; s++) {
                    if (d < nearest_d[s]) {
                        for (int j = 2; j > s; j--) { nearest[j] = nearest[j-1]; nearest_d[j] = nearest_d[j-1]; }
                        nearest[s] = ci;
                        nearest_d[s] = d;
                        break;
                    }
                }
            }
            for (int s = 0; s < 3 && slot_count < 3; s++) {
                if (nearest[s] >= 0) slot_contract[slot_count++] = nearest[s];
            }
        }

        /* [1/2/3] select a contract slot. */
        for (int k = 0; k < 3; k++) {
            if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
            if (slot_contract[k] < 0) break;
            g.selected_contract = slot_contract[k];
            g.tracked_contract = slot_contract[k];
            const contract_t *ct = &g.world.contracts[slot_contract[k]];
            set_notice("Selected: %s. [E] deliver.",
                       commodity_short_name(ct->commodity));
            break;
        }

        /* [E] deliver. Selective if a slot is selected, else everything. */
        if (is_key_pressed(SAPP_KEYCODE_E)) {
            if (g.selected_contract >= 0 && g.selected_contract < MAX_CONTRACTS) {
                const contract_t *ct = &g.world.contracts[g.selected_contract];
                if (ct->active) {
                    intent.service_sell = true;
                    intent.service_sell_only = ct->commodity;
                    set_notice("Delivering %s...",
                               commodity_short_name(ct->commodity));
                } else {
                    /* Selected contract was completed/cancelled; fall back. */
                    intent.service_sell = true;
                    intent.service_sell_only = COMMODITY_COUNT;
                    set_notice("Delivering all matching cargo...");
                }
                g.selected_contract = -1;
            } else {
                intent.service_sell = true;
                intent.service_sell_only = COMMODITY_COUNT;
                set_notice("Delivering all matching cargo...");
            }
        }
    } else {
        /* Default: service keys */
        intent.service_sell = is_key_pressed(SAPP_KEYCODE_1);
        if (intent.service_sell) {
            const station_t *sell_st = current_station_ptr();
            bool delivered_to_scaffold = false;
            /* Check if we can deliver to any scaffold module */
            if (sell_st) {
                for (int mi = 0; mi < sell_st->module_count; mi++) {
                    if (!sell_st->modules[mi].scaffold) continue;
                    commodity_t mat = module_build_material_lookup(sell_st->modules[mi].type);
                    if (LOCAL_PLAYER.ship.cargo[mat] > 0.01f) {
                        set_notice("Delivering to %s scaffold.", module_type_name(sell_st->modules[mi].type));
                        delivered_to_scaffold = true;
                        break;
                    }
                }
            }
            if (!delivered_to_scaffold) {
                /* Optimistic prediction: deliver primary buy commodity */
                float est_payout = 0.0f;
                if (sell_st) {
                    commodity_t buy = station_primary_buy(sell_st);
                    if ((int)buy >= 0 && LOCAL_PLAYER.ship.cargo[buy] > FLOAT_EPSILON) {
                        float capacity = (buy < COMMODITY_RAW_ORE_COUNT)
                            ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
                        float space = fmaxf(0.0f, capacity - sell_st->inventory[buy]);
                        float sellable = fminf(LOCAL_PLAYER.ship.cargo[buy], space);
                        est_payout = sellable * station_buy_price(sell_st, buy);
                        LOCAL_PLAYER.ship.cargo[buy] -= sellable;
                        LOCAL_PLAYER.ship.credits += est_payout;
                    }
                }
                if (est_payout > FLOAT_EPSILON) {
                    set_notice("Delivered  +%d cr", (int)lroundf(est_payout));
                } else {
                    set_notice("Nothing to deliver here.");
                }
            }
        }
        intent.service_repair = is_key_pressed(SAPP_KEYCODE_2);
        intent.upgrade_mining = is_key_pressed(SAPP_KEYCODE_3);
        intent.upgrade_hold = is_key_pressed(SAPP_KEYCODE_4);
        intent.upgrade_tractor = is_key_pressed(SAPP_KEYCODE_5);
    }
    /* Buy product from station (F key while docked) */
    if (LOCAL_PLAYER.docked && is_key_pressed(SAPP_KEYCODE_F)) {
        const station_t *st = current_station_ptr();
        if (st) {
            commodity_t sell = station_primary_sell(st);
            if ((int)sell >= 0 && st->inventory[sell] > 0.5f && st->base_price[sell] > FLOAT_EPSILON) {
                float space = ship_cargo_capacity(&LOCAL_PLAYER.ship) - ship_total_cargo(&LOCAL_PLAYER.ship);
                float price = station_sell_price(st, sell);
                if (space < 0.5f) {
                    set_notice("Hold full.");
                } else if (LOCAL_PLAYER.ship.credits < price) {
                    set_notice("Need %d cr.", (int)lroundf(price));
                } else {
                    float avail = st->inventory[sell];
                    float afford = floorf(LOCAL_PLAYER.ship.credits / price);
                    int amount = (int)fminf(fminf(avail, space), afford);
                    intent.buy_product = true;
                    intent.buy_commodity = sell;
                    LOCAL_PLAYER.ship.cargo[sell] += (float)amount;
                    LOCAL_PLAYER.ship.credits -= (float)amount * price;
                    set_notice("Bought %d %s  -%d cr", amount, commodity_short_name(sell), (int)(amount * price));
                }
            } else {
                set_notice("Nothing to buy here.");
            }
        }
    }
    /* B / R / E: placement (tow mode) and planning (plan mode).
     * Tow mode: position auto-picks slot, E commits, no reticle.
     * Plan mode: position auto-picks slot, R cycles type, E reserves slot.
     * B enters/exits plan mode. Plans are server-side. */
    if (!LOCAL_PLAYER.docked && LOCAL_PLAYER.ship.towed_scaffold >= 0) {
        /* TOW MODE: server snaps to closest slot on E. */
        g.placement_reticle_active = false;
        if (is_key_pressed(SAPP_KEYCODE_E)) {
            reticle_target_t targets[RETICLE_MAX_TARGETS];
            int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
            if (n > 0) {
                intent.place_outpost = true;
                intent.place_target_station = (int8_t)targets[0].station;
                intent.place_target_ring = (int8_t)targets[0].ring;
                intent.place_target_slot = (int8_t)targets[0].slot;
                set_notice("Placing scaffold...");
            } else {
                /* No outpost in range — let server decide:
                 * - materialize a nearby planned station, or
                 * - found a new outpost from scratch */
                intent.place_outpost = true;
                set_notice("Placing scaffold...");
            }
        }
    } else if (g.plan_mode_active) {
        /* PLAN MODE: cycle types with R, place with E, exit with B/Esc.
         *
         * Two sub-modes:
         *   plan_target_station == -1  →  GHOST PREVIEW: rings draw
         *     around the player's ship; nothing committed server-side yet.
         *   plan_target_station >= 3   →  REAL: targeting a server-side
         *     planned station (created by E in ghost mode or by legacy path).
         */
        bool ghost_mode = (g.plan_target_station == -1);

        if (!ghost_mode) {
            /* Real station: find reticle targets normally. */
            reticle_target_t targets[RETICLE_MAX_TARGETS];
            int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
            if (n == 0) {
                if (g.world.time >= g.plan_mode_grace_until) {
                    g.plan_mode_active = false;
                }
            } else {
                g.placement_target_station = targets[0].station;
                g.placement_target_ring = targets[0].ring;
                g.placement_target_slot = targets[0].slot;
                g.plan_mode_grace_until = 0.0f;
            }
        } else {
            /* Ghost mode: pick the slot closest to the ship's forward. */
            vec2 fwd = v2_from_angle(LOCAL_PLAYER.ship.angle);
            float best_dot = -2.0f;
            int best_ring = 1, best_slot = 0;
            for (int ring = 1; ring <= 1; ring++) { /* ghost starts with ring 1 only */
                int slots_n = STATION_RING_SLOTS[ring];
                for (int slot = 0; slot < slots_n; slot++) {
                    float angle = TWO_PI_F * (float)slot / (float)slots_n;
                    vec2 dir = v2(cosf(angle), sinf(angle));
                    float d = v2_dot(fwd, dir);
                    if (d > best_dot) { best_dot = d; best_ring = ring; best_slot = slot; }
                }
            }
            g.placement_target_station = -1;
            g.placement_target_ring = best_ring;
            g.placement_target_slot = best_slot;
        }

        if (is_key_pressed(SAPP_KEYCODE_ESCAPE) || is_key_pressed(SAPP_KEYCODE_B)) {
            if (!ghost_mode) {
                int s = g.placement_target_station;
                if (s >= 3 && s < MAX_STATIONS &&
                    g.world.stations[s].planned &&
                    g.world.stations[s].placement_plan_count == 0) {
                    intent.cancel_planned_outpost = true;
                    intent.cancel_planned_station = (int8_t)s;
                    set_notice("Outpost design cancelled.");
                }
            }
            g.plan_mode_active = false;
        } else if (g.plan_mode_active) {
            if (is_key_pressed(SAPP_KEYCODE_R)) {
                static const module_type_t plannable[] = {
                    MODULE_FURNACE, MODULE_FURNACE_CU, MODULE_FURNACE_CR,
                    MODULE_FRAME_PRESS, MODULE_LASER_FAB, MODULE_TRACTOR_FAB,
                    MODULE_ORE_BUYER, MODULE_ORE_SILO, MODULE_CARGO_BAY,
                    MODULE_REPAIR_BAY, MODULE_SIGNAL_RELAY, MODULE_DOCK,
                    MODULE_SHIPYARD,
                };
                int count = (int)(sizeof(plannable)/sizeof(plannable[0]));
                module_type_t planned[PLAYER_PLAN_TYPE_LIMIT];
                int planned_n = player_planned_types(planned, PLAYER_PLAN_TYPE_LIMIT);
                uint32_t mask = LOCAL_PLAYER.ship.unlocked_modules;
                int cur = 0;
                for (int i = 0; i < count; i++)
                    if ((int)plannable[i] == g.plan_type) { cur = i; break; }
                int next = -1;
                for (int step = 1; step <= count; step++) {
                    int idx = (cur + step) % count;
                    module_type_t t = plannable[idx];
                    if (!module_unlocked_for_player(mask, t)) continue;
                    if (planned_n >= PLAYER_PLAN_TYPE_LIMIT) {
                        bool match = false;
                        for (int k = 0; k < planned_n; k++)
                            if (planned[k] == t) { match = true; break; }
                        if (!match) continue;
                    }
                    next = (int)t;
                    break;
                }
                if (next >= 0) g.plan_type = next;
                intent.release_tow = false;
            } else if (is_key_pressed(SAPP_KEYCODE_E)) {
                module_type_t planned[PLAYER_PLAN_TYPE_LIMIT];
                int planned_n = player_planned_types(planned, PLAYER_PLAN_TYPE_LIMIT);
                bool already = false;
                for (int k = 0; k < planned_n; k++)
                    if (planned[k] == (module_type_t)g.plan_type) { already = true; break; }
                if (!module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules,
                                                (module_type_t)g.plan_type)) {
                    set_notice("%s is locked.", module_type_name((module_type_t)g.plan_type));
                } else if (!already && planned_n >= PLAYER_PLAN_TYPE_LIMIT) {
                    set_notice("Plan limit %d types. Cancel one first.",
                        PLAYER_PLAN_TYPE_LIMIT);
                } else if (ghost_mode) {
                    /* Ghost → lock: create planned outpost + first plan
                     * in one atomic message. The station materializes at
                     * the player's current ship position. */
                    vec2 pos = LOCAL_PLAYER.ship.pos;
                    bool too_close = false;
                    for (int s = 0; s < MAX_STATIONS; s++) {
                        const station_t *st = &g.world.stations[s];
                        if (!station_exists(st)) continue;
                        if (v2_dist_sq(st->pos, pos) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) {
                            too_close = true; break;
                        }
                    }
                    if (too_close) {
                        set_notice("Too close to an existing station.");
                    } else if (signal_strength_at(&g.world, pos) <= 0.0f) {
                        set_notice("No signal here.");
                    } else {
                        /* Atomic create + first plan */
                        intent.create_planned_outpost = true;
                        intent.planned_outpost_pos = pos;
                        intent.add_plan = true;
                        intent.plan_station = -2; /* sentinel: just-created */
                        intent.plan_ring = (int8_t)g.placement_target_ring;
                        intent.plan_slot = (int8_t)g.placement_target_slot;
                        intent.plan_type = (module_type_t)g.plan_type;
                        /* Lock effect */
                        g.outpost_lock_timer = 1.5f;
                        g.outpost_lock_pos = pos;
                        /* Transition to grace mode — wait for server to
                         * send back the created station, then switch to
                         * real plan mode targeting it. */
                        g.plan_mode_grace_until = g.world.time + 1.5f;
                        set_notice("Station locked! [R] type [E] place [B] exit");
                    }
                } else {
                    /* Real station: check if this slot already has a plan.
                     * If so, E clears it (cancel). Otherwise E adds. */
                    int ps = g.placement_target_station;
                    int pr = g.placement_target_ring;
                    int psl = g.placement_target_slot;
                    bool has_existing = false;
                    if (ps >= 0 && ps < MAX_STATIONS) {
                        const station_t *pst = &g.world.stations[ps];
                        for (int p = 0; p < pst->placement_plan_count; p++) {
                            if (pst->placement_plans[p].ring == pr &&
                                pst->placement_plans[p].slot == psl) {
                                has_existing = true; break;
                            }
                        }
                    }
                    if (has_existing) {
                        intent.cancel_plan_slot = true;
                        intent.cancel_plan_st = (int8_t)ps;
                        intent.cancel_plan_ring = (int8_t)pr;
                        intent.cancel_plan_sl = (int8_t)psl;
                        set_notice("Plan cleared. [R] type [E] place [B] exit");
                    } else {
                        intent.add_plan = true;
                        intent.plan_station = (int8_t)ps;
                        intent.plan_ring = (int8_t)pr;
                        intent.plan_slot = (int8_t)psl;
                        intent.plan_type = (module_type_t)g.plan_type;
                        set_notice("Planned %s. [R] type [E] place [B] exit",
                            module_type_name((module_type_t)g.plan_type));
                    }
                }
            }
        }
    } else if (is_key_pressed(SAPP_KEYCODE_B)) {
        if (LOCAL_PLAYER.docked) {
            const station_t *st = current_station_ptr();
            if (st && station_has_module(st, MODULE_SHIPYARD)) {
                g.station_tab = STATION_TAB_SHIPYARD;
            } else {
                set_notice("No shipyard here.");
            }
        } else {
            /* Undocked, not towing.
             * Near an existing outpost or planned station → enter plan
             * mode targeting it. Otherwise → enter ghost preview mode
             * (rings follow ship, nothing committed until E). */
            reticle_target_t targets[RETICLE_MAX_TARGETS];
            int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
            uint32_t mask = LOCAL_PLAYER.ship.unlocked_modules;
            if (g.plan_type == 0 || g.plan_type == MODULE_DOCK ||
                !module_unlocked_for_player(mask, (module_type_t)g.plan_type)) {
                g.plan_type = MODULE_SIGNAL_RELAY;
            }
            if (n > 0) {
                g.plan_mode_active = true;
                g.placement_target_station = targets[0].station;
                g.placement_target_ring = targets[0].ring;
                g.placement_target_slot = targets[0].slot;
                g.plan_target_station = targets[0].station;
                set_notice("Plan: [R] type [E] place [B] exit");
            } else {
                /* Ghost preview: rings follow the ship, no server message. */
                g.plan_mode_active = true;
                g.plan_target_station = -1; /* sentinel: ghost */
                g.placement_target_station = -1;
                g.placement_target_ring = 1;
                g.placement_target_slot = 0;
                set_notice("Plan: [R] type [E] lock [B] exit");
            }
        }
    }

    /* [ ] keys: prev/next track */
    if (is_key_pressed(SAPP_KEYCODE_LEFT_BRACKET)) {
        music_prev_track(&g.music);
        const music_track_info_t *info = music_get_info(g.music.current_track);
        if (info) set_notice("%s", info->title);
    }
    if (is_key_pressed(SAPP_KEYCODE_RIGHT_BRACKET)) {
        music_next_track(&g.music);
        const music_track_info_t *info = music_get_info(g.music.current_track);
        if (info) set_notice("%s", info->title);
    }
    /* H key: hail nearby station to collect pending credits */
    if (is_key_pressed(SAPP_KEYCODE_H) && !LOCAL_PLAYER.docked) {
        intent.hail = true;
    }
    /* O key: toggle mining autopilot — server-side AI runs the mining
     * loop on the player's ship. Any manual movement or mine input
     * cancels it. Works docked or undocked. */
    if (is_key_pressed(SAPP_KEYCODE_O)) {
        if (!LOCAL_PLAYER.autopilot_mode) {
            float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
            if (sig < SIGNAL_BAND_OPERATIONAL) {
                set_notice("Signal too weak for autopilot.");
            } else {
                intent.toggle_autopilot = true;
            }
        } else {
            intent.toggle_autopilot = true; /* always allow turning off */
        }
    }
    return intent;
}

void submit_input(const input_intent_t *intent, float dt) {
    /* Set on client world for prediction */
    LOCAL_PLAYER.input = *intent;

    /* Client prediction: immediate local feedback (movement, beam targeting) */
    world_sim_step_player_only(&g.world, g.local_player_slot, dt);

    /* Authoritative step: local server or remote */
    if (g.local_server.active) {
        /* Forward client's predicted target so server damages the same asteroid */
        input_intent_t server_intent = *intent;
        server_intent.mining_target_hint = LOCAL_PLAYER.hover_asteroid;
        local_server_step(&g.local_server, g.local_player_slot, &server_intent, dt);
        local_server_sync_to_client(&g.local_server);
    }


    /* Detect one-shot actions for prediction suppression and network send */
    bool has_action = intent->interact || intent->service_sell ||
        intent->service_repair || intent->upgrade_mining ||
        intent->upgrade_hold || intent->upgrade_tractor ||
        intent->place_outpost || intent->buy_scaffold_kit ||
        intent->buy_product || intent->hail ||
        intent->release_tow || intent->add_plan ||
        intent->create_planned_outpost || intent->cancel_planned_outpost ||
        intent->cancel_plan_slot || intent->toggle_autopilot;

    if (has_action)
        g.action_predict_timer = 0.5f;

    /* Multiplayer: plan intents ride a dedicated message — they carry
     * richer payloads (target station/ring/slot/type or world position)
     * that don't fit in the 1-byte action slot. Send them directly. */
    if (g.multiplayer_enabled && net_is_connected()) {
        if (intent->create_planned_outpost && intent->add_plan &&
            intent->plan_station == -2) {
            /* Atomic create + first plan — single message. */
            net_send_plan(NET_PLAN_OP_CREATE_AND_ADD,
                          -1,
                          intent->plan_ring,
                          intent->plan_slot,
                          (uint8_t)intent->plan_type,
                          intent->planned_outpost_pos.x,
                          intent->planned_outpost_pos.y);
        } else {
            if (intent->create_planned_outpost) {
                net_send_plan(NET_PLAN_OP_CREATE_OUTPOST,
                              -1, -1, -1, 0,
                              intent->planned_outpost_pos.x,
                              intent->planned_outpost_pos.y);
            }
            if (intent->add_plan) {
                net_send_plan(NET_PLAN_OP_ADD_SLOT,
                              intent->plan_station,
                              intent->plan_ring,
                              intent->plan_slot,
                              (uint8_t)intent->plan_type,
                              0.0f, 0.0f);
            }
        }
        if (intent->cancel_planned_outpost) {
            net_send_plan(NET_PLAN_OP_CANCEL_OUTPOST,
                          intent->cancel_planned_station,
                          -1, -1, 0,
                          0.0f, 0.0f);
        }
        if (intent->cancel_plan_slot) {
            net_send_plan(NET_PLAN_OP_CANCEL_PLAN_SLOT,
                          intent->cancel_plan_st,
                          intent->cancel_plan_ring,
                          intent->cancel_plan_sl,
                          0, 0.0f, 0.0f);
        }
    }

    /* Multiplayer: encode the action and queue for network send */
    if (has_action && g.multiplayer_enabled && net_is_connected()) {
        if (intent->interact) {
            g.pending_net_action = LOCAL_PLAYER.docked ? 2 : 1;
            if (LOCAL_PLAYER.docked) {
                LOCAL_PLAYER.docked = false;
                LOCAL_PLAYER.in_dock_range = false;
            }
        } else if (intent->service_sell && intent->service_sell_only < COMMODITY_COUNT)
            g.pending_net_action = NET_ACTION_DELIVER_COMMODITY + (uint8_t)intent->service_sell_only;
        else if (intent->service_sell)
            g.pending_net_action = 3;
        else if (intent->service_repair)
            g.pending_net_action = 4;
        else if (intent->upgrade_mining)
            g.pending_net_action = 5;
        else if (intent->upgrade_hold)
            g.pending_net_action = 6;
        else if (intent->upgrade_tractor)
            g.pending_net_action = 7;
        else if (intent->place_outpost)
            g.pending_net_action = 8;
        else if (intent->buy_scaffold_kit && (uint8_t)intent->scaffold_kit_module < MODULE_COUNT)
            g.pending_net_action = NET_ACTION_BUY_SCAFFOLD_TYPED + (uint8_t)intent->scaffold_kit_module;
        else if (intent->buy_product && (uint8_t)intent->buy_commodity < COMMODITY_COUNT)
            g.pending_net_action = NET_ACTION_BUY_PRODUCT + (uint8_t)intent->buy_commodity;
        else if (intent->hail)
            g.pending_net_action = NET_ACTION_HAIL;
        else if (intent->release_tow)
            g.pending_net_action = NET_ACTION_RELEASE_TOW;
        else if (intent->reset)
            g.pending_net_action = NET_ACTION_RESET;
        else if (intent->toggle_autopilot)
            g.pending_net_action = NET_ACTION_AUTOPILOT_TOGGLE;
    }
}
