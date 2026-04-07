/*
 * input.c -- Input handling for the Signal Space Miner client.
 */
#include <stdarg.h>
#include "input.h"
#include "local_server.h"
#include "music.h"
#include "net.h"

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

    intent.mine = is_key_down(SAPP_KEYCODE_SPACE);
    intent.release_tow = is_key_pressed(SAPP_KEYCODE_R);
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
    /* E key: activate targeted module, or dock/launch if no target */
    if (is_key_pressed(SAPP_KEYCODE_E)) {
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
        /* Shipyard tab: 1-9 order a scaffold */
        static const module_type_t sellable[] = {
            MODULE_DOCK, MODULE_SIGNAL_RELAY, MODULE_FURNACE,
            MODULE_ORE_BUYER, MODULE_ORE_SILO, MODULE_FRAME_PRESS,
            MODULE_FURNACE_CU, MODULE_FURNACE_CR,
            MODULE_LASER_FAB, MODULE_TRACTOR_FAB,
            MODULE_CARGO_BAY, MODULE_REPAIR_BAY,
        };
        int shown = 0;
        for (int si = 0; si < (int)(sizeof(sellable)/sizeof(sellable[0])); si++) {
            if (!station_has_module(st, sellable[si])) continue;
            if (is_key_pressed(SAPP_KEYCODE_1 + shown)) {
                if (st->pending_scaffold_count >= 4) {
                    set_notice("Shipyard queue full.");
                } else if ((int)lroundf(LOCAL_PLAYER.ship.credits) < scaffold_order_fee(sellable[si])) {
                    set_notice("Need %d cr to order.", scaffold_order_fee(sellable[si]));
                } else {
                    intent.buy_scaffold_kit = true;
                    intent.scaffold_kit_module = sellable[si];
                    set_notice("Ordered %s scaffold.", module_type_name(sellable[si]));
                }
                break;
            }
            shown++;
        }
    } else if (LOCAL_PLAYER.docked && g.station_tab == STATION_TAB_CONTRACTS) {
        /* Contracts tab: 1/2/3 track contract */
        for (int k = 0; k < 3; k++) {
            if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
            int nearest[3] = {-1, -1, -1};
            float nearest_d[3] = {1e18f, 1e18f, 1e18f};
            const station_t *here_st = current_station_ptr();
            if (!here_st) break;
            vec2 here = here_st->pos;
            for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
                contract_t *ct = &g.world.contracts[ci];
                if (!ct->active || ct->station_index >= MAX_STATIONS) continue;
                if (!station_exists(&g.world.stations[ct->station_index])) continue;
                vec2 target = (ct->action == CONTRACT_TRACTOR) ? g.world.stations[ct->station_index].pos : ct->target_pos;
                float d = v2_dist_sq(here, target);
                for (int slot = 0; slot < 3; slot++) {
                    if (d < nearest_d[slot]) {
                        for (int j = 2; j > slot; j--) { nearest[j] = nearest[j-1]; nearest_d[j] = nearest_d[j-1]; }
                        nearest[slot] = ci;
                        nearest_d[slot] = d;
                        break;
                    }
                }
            }
            if (nearest[k] >= 0) {
                g.tracked_contract = nearest[k];
                set_notice("Contract tracked.");
            }
            break;
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
         * Targets a server-side station (player outpost or planned). */
        reticle_target_t targets[RETICLE_MAX_TARGETS];
        int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);

        if (n == 0) {
            g.plan_mode_active = false;
        } else {
            g.placement_target_station = targets[0].station;
            g.placement_target_ring = targets[0].ring;
            g.placement_target_slot = targets[0].slot;
        }

        if (is_key_pressed(SAPP_KEYCODE_ESCAPE) || is_key_pressed(SAPP_KEYCODE_B)) {
            /* Exiting plan mode. If the target is a planned station with
             * no slots reserved yet, cancel it entirely so B+B is a clean
             * "never mind" gesture instead of a flicker. */
            int s = g.placement_target_station;
            if (s >= 3 && s < MAX_STATIONS &&
                g.world.stations[s].planned &&
                g.world.stations[s].placement_plan_count == 0) {
                intent.cancel_planned_outpost = true;
                intent.cancel_planned_station = (int8_t)s;
                set_notice("Outpost design cancelled.");
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
                int cur = 0;
                for (int i = 0; i < count; i++)
                    if ((int)plannable[i] == g.plan_type) { cur = i; break; }
                g.plan_type = (int)plannable[(cur + 1) % count];
                intent.release_tow = false;
            } else if (is_key_pressed(SAPP_KEYCODE_E)) {
                intent.add_plan = true;
                intent.plan_station = (int8_t)g.placement_target_station;
                intent.plan_ring = (int8_t)g.placement_target_ring;
                intent.plan_slot = (int8_t)g.placement_target_slot;
                intent.plan_type = (module_type_t)g.plan_type;
                set_notice("Planned %s. [R] type [E] place [B] exit",
                    module_type_name((module_type_t)g.plan_type));
            }
        }
    } else if (is_key_pressed(SAPP_KEYCODE_B)) {
        if (LOCAL_PLAYER.docked) {
            /* B shortcut: jump to shipyard tab if available */
            const station_t *st = current_station_ptr();
            if (st && station_has_module(st, MODULE_SHIPYARD)) {
                g.station_tab = STATION_TAB_SHIPYARD;
            } else {
                set_notice("No shipyard here.");
            }
        } else {
            /* Undocked, not towing.
             * Near an existing outpost or planned station → enter plan mode.
             * Otherwise → ask the server to create a planned outpost. */
            reticle_target_t targets[RETICLE_MAX_TARGETS];
            int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
            if (n > 0) {
                g.plan_mode_active = true;
                g.placement_target_station = targets[0].station;
                g.placement_target_ring = targets[0].ring;
                g.placement_target_slot = targets[0].slot;
                g.plan_target_station = targets[0].station;
                if (g.plan_type == 0) g.plan_type = MODULE_FURNACE;
                set_notice("Plan: [R] type [E] place [B] exit");
            } else {
                /* No outpost in range — request a planned outpost from server */
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
                    set_notice("No signal here. Move closer to a station.");
                } else {
                    intent.create_planned_outpost = true;
                    intent.planned_outpost_pos = pos;
                    if (g.plan_type == 0) g.plan_type = MODULE_FURNACE;
                    /* Plan mode will activate next frame when the new
                     * planned station shows up in collect_reticle_targets. */
                    set_notice("Designing outpost...");
                }
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
        intent->create_planned_outpost || intent->cancel_planned_outpost;

    if (has_action)
        g.action_predict_timer = 0.5f;

    /* Multiplayer: encode the action and queue for network send */
    if (has_action && g.multiplayer_enabled && net_is_connected()) {
        if (intent->interact) {
            g.pending_net_action = LOCAL_PLAYER.docked ? 2 : 1;
            if (LOCAL_PLAYER.docked) {
                LOCAL_PLAYER.docked = false;
                LOCAL_PLAYER.in_dock_range = false;
            }
        } else if (intent->service_sell)
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
    }
}
