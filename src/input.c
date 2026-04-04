/*
 * input.c -- Input handling for the Signal Space Miner client.
 */
#include <stdarg.h>
#include "input.h"
#include "local_server.h"
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

input_intent_t sample_input_intent(void) {
    input_intent_t intent = { 0 };

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
    /* Safety: close build overlay if not docked */
    if (!LOCAL_PLAYER.docked) g.build_overlay = false;
    /* E key: cycle module targets when near station, else dock/launch */
    if (is_key_pressed(SAPP_KEYCODE_E)) {
        if (!LOCAL_PLAYER.docked && LOCAL_PLAYER.in_dock_range && LOCAL_PLAYER.nearby_station >= 0) {
            /* Cycle through modules on the nearby station */
            const station_t *st = &g.world.stations[LOCAL_PLAYER.nearby_station];
            int start = (g.target_station == LOCAL_PLAYER.nearby_station) ? g.target_module + 1 : 0;
            g.target_station = LOCAL_PLAYER.nearby_station;
            g.target_module = -1;
            float tr = ship_tractor_range(&LOCAL_PLAYER.ship);
            float tr_sq = tr * tr;
            for (int tries = 0; tries < st->module_count; tries++) {
                int idx = (start + tries) % st->module_count;
                if (st->modules[idx].scaffold) continue;
                vec2 mp = module_world_pos_ring(st, st->modules[idx].ring, st->modules[idx].slot);
                if (v2_dist_sq(LOCAL_PLAYER.ship.pos, mp) <= tr_sq) {
                    g.target_module = idx;
                    break;
                }
            }
            if (g.target_module < 0) {
                /* No modules in range — fall back to dock/interact */
                g.target_station = -1;
                intent.interact = true;
            }
        } else {
            /* Not near station or docked: dock/launch */
            intent.interact = true;
            g.target_station = -1;
            g.target_module = -1;
        }
        g.build_overlay = false;
        g.placing_outpost = false;
    }
    /* Clear target if we moved out of range */
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
    /* Number keys: context-dependent */
    if (LOCAL_PLAYER.docked && g.build_overlay) {
        const station_t *st = current_station_ptr();
        /* 1-8: pick module type, auto-placed at next slot */
        static const struct { module_type_t type; const char *name; } build_keys[] = {
            { MODULE_FURNACE,      "Furnace (FE)" },
            { MODULE_FURNACE_CU,   "Furnace (CU)" },
            { MODULE_FURNACE_CR,   "Furnace (CR)" },
            { MODULE_FRAME_PRESS,  "Frame Press" },
            { MODULE_LASER_FAB,    "Laser Fab" },
            { MODULE_TRACTOR_FAB,  "Tractor Fab" },
            { MODULE_ORE_BUYER,    "Ore Buyer" },
            { MODULE_SIGNAL_RELAY, "Signal Relay" },
        };
        for (int k = 0; k < 8; k++) {
            if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
            if (st->module_count >= MAX_MODULES_PER_STATION) {
                set_notice("Station is full.");
            } else {
                intent.build_module = true;
                intent.build_module_type = build_keys[k].type;
                intent.build_ring = 1;
                intent.build_slot = (uint8_t)st->module_count;
                set_notice("Building %s", build_keys[k].name);
                g.build_overlay = false;
            }
            break;
        }
        if (is_key_pressed(SAPP_KEYCODE_ESCAPE) || is_key_pressed(SAPP_KEYCODE_B)
            || is_key_pressed(SAPP_KEYCODE_TAB))
            g.build_overlay = false;
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
                vec2 target = (ct->action == CONTRACT_SUPPLY) ? g.world.stations[ct->station_index].pos : ct->target_pos;
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
            /* Optimistic prediction: deliver primary buy commodity */
            const station_t *sell_st = current_station_ptr();
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
    /* B key: build mode */
    if (g.placing_outpost) {
        /* Outpost placement: B/Enter confirms, Esc cancels */
        if (is_key_pressed(SAPP_KEYCODE_B) || is_key_pressed(SAPP_KEYCODE_ENTER) || is_key_pressed(SAPP_KEYCODE_KP_ENTER)) {
            intent.place_outpost = true;
            g.placing_outpost = false;
            vec2 fwd = v2_from_angle(LOCAL_PLAYER.ship.angle);
            g.nav_pip_active = true;
            g.nav_pip_pos = v2_add(LOCAL_PLAYER.ship.pos, v2_scale(fwd, 150.0f));
            g.nav_pip_is_blueprint = true;
        } else if (is_key_pressed(SAPP_KEYCODE_ESCAPE) || is_key_pressed(SAPP_KEYCODE_Q)) {
            g.placing_outpost = false;
        }
    } else if (is_key_pressed(SAPP_KEYCODE_B)) {
        if (LOCAL_PLAYER.docked) {
            if (g.build_overlay) {
                g.build_overlay = false;
            } else if (!LOCAL_PLAYER.ship.has_scaffold_kit) {
                /* Buy scaffold kit */
                const station_t *st = current_station_ptr();
                if (st && station_has_module(st, MODULE_BLUEPRINT_DESK)) {
                    if (LOCAL_PLAYER.ship.credits >= OUTPOST_CREDIT_COST) {
                        intent.buy_scaffold_kit = true;
                        set_notice("Scaffold kit purchased. Undock and press B to deploy.");
                    } else {
                        set_notice("Need %d cr for scaffold kit.", (int)OUTPOST_CREDIT_COST);
                    }
                } else {
                    set_notice("No blueprint desk here.");
                }
            } else {
                g.build_overlay = true;
                g.build_ring = 1;
                g.build_slot = -1;
            }
        } else {
            if (LOCAL_PLAYER.ship.has_scaffold_kit) {
                g.placing_outpost = true;
            } else {
                set_notice("Buy a scaffold kit at a station first.");
            }
        }
    }
    /* H key: hail nearby station to collect pending credits */
    if (is_key_pressed(SAPP_KEYCODE_H) && !LOCAL_PLAYER.docked) {
        intent.hail = true;
    }
    intent.reset = is_key_pressed(SAPP_KEYCODE_R);
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
        intent->build_module || intent->buy_product || intent->hail;

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
        else if (intent->buy_scaffold_kit)
            g.pending_net_action = NET_ACTION_BUY_SCAFFOLD;
        else if (intent->build_module && (uint8_t)intent->build_module_type < MODULE_COUNT)
            g.pending_net_action = NET_ACTION_BUILD_MODULE + (uint8_t)intent->build_module_type;
        else if (intent->buy_product && (uint8_t)intent->buy_commodity < COMMODITY_COUNT)
            g.pending_net_action = NET_ACTION_BUY_PRODUCT + (uint8_t)intent->buy_commodity;
        else if (intent->hail)
            g.pending_net_action = NET_ACTION_HAIL;
    }
}
