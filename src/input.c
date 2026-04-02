/*
 * input.c -- Input handling for the Signal Space Miner client.
 */
#include <stdarg.h>
#include "input.h"
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
    /* Safety: close build overlay if not docked */
    if (!LOCAL_PLAYER.docked) g.build_overlay = false;
    /* E key: interact (dock/launch) */
    intent.interact = is_key_pressed(SAPP_KEYCODE_E);
    if (intent.interact) {
        g.build_overlay = false;
        g.placing_outpost = false;
    }
    /* Number keys: context-dependent */
    if (LOCAL_PLAYER.docked && g.build_overlay) {
        /* Build overlay: 1-8 select module, Esc/B closes */
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
            const station_t *st = current_station_ptr();
            if (station_has_module(st, build_keys[k].type)) {
                set_notice("%s already installed.", build_keys[k].name);
            } else if (st->module_count >= MAX_MODULES_PER_STATION) {
                set_notice("No module slots available.");
            } else {
                intent.build_module = true;
                intent.build_module_type = build_keys[k].type;
                set_notice("Blueprint placed: %s", build_keys[k].name);
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
        if (intent.service_sell && ship_total_cargo(&LOCAL_PLAYER.ship) > 0.01f) {
            /* Optimistic prediction: clear cargo and estimate payout */
            const station_t *sell_st = current_station_ptr();
            float est_payout = 0.0f;
            if (sell_st) {
                for (int c = 0; c < COMMODITY_COUNT; c++) {
                    float amt = LOCAL_PLAYER.ship.cargo[c];
                    if (amt < 0.01f) continue;
                    if (c < COMMODITY_RAW_ORE_COUNT && (sell_st->services & STATION_SERVICE_ORE_BUYER)) {
                        float hopper_space = REFINERY_HOPPER_CAPACITY - sell_st->inventory[c];
                        float sellable = fminf(amt, fmaxf(0.0f, hopper_space));
                        est_payout += sellable * station_buy_price(sell_st, (commodity_t)c);
                        LOCAL_PLAYER.ship.cargo[c] -= sellable;
                    }
                    /* Estimate contract delivery payout without mutating contracts */
                    for (int k = 0; k < MAX_CONTRACTS && c >= COMMODITY_RAW_ORE_COUNT; k++) {
                        const contract_t *ct = &g.world.contracts[k];
                        if (!ct->active || ct->action != CONTRACT_SUPPLY) continue;
                        if (ct->station_index != LOCAL_PLAYER.current_station) continue;
                        if (ct->commodity != (commodity_t)c) continue;
                        float deliver = fminf(amt, ct->quantity_needed);
                        LOCAL_PLAYER.ship.cargo[c] -= deliver;
                        est_payout += deliver * ct->base_price;
                        break;
                    }
                }
                LOCAL_PLAYER.ship.credits += est_payout;
            }
            set_notice("Sold cargo  +%d cr", (int)lroundf(est_payout));
        }
        intent.service_repair = is_key_pressed(SAPP_KEYCODE_2);
        intent.upgrade_mining = is_key_pressed(SAPP_KEYCODE_3);
        intent.upgrade_hold = is_key_pressed(SAPP_KEYCODE_4);
        intent.upgrade_tractor = is_key_pressed(SAPP_KEYCODE_5);
    }
    /* Buy ingots from station (F key while docked) */
    if (LOCAL_PLAYER.docked && is_key_pressed(SAPP_KEYCODE_F)) {
        const station_t *st = current_station_ptr();
        if (st) {
            /* Buy the first available ingot type */
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                if (st->inventory[c] > 0.5f && st->buy_price[c] > 0.01f) {
                    float space = ship_cargo_capacity(&LOCAL_PLAYER.ship) - ship_total_cargo(&LOCAL_PLAYER.ship);
                    float price = station_buy_price(st, (commodity_t)c);
                    if (space < 0.5f) {
                        set_notice("Hold full.");
                    } else if (LOCAL_PLAYER.ship.credits < price) {
                        set_notice("Need %d cr.", (int)lroundf(price));
                    } else {
                        float avail = st->inventory[c];
                        float afford = floorf(LOCAL_PLAYER.ship.credits / price);
                        int amount = (int)fminf(fminf(avail, space), afford);
                        intent.buy_product = true;
                        intent.buy_commodity = (commodity_t)c;
                        /* Optimistic client prediction */
                        LOCAL_PLAYER.ship.cargo[c] += (float)amount;
                        LOCAL_PLAYER.ship.credits -= (float)amount * price;
                        set_notice("Bought %d %s  -%d cr", amount, commodity_short_name((commodity_t)c), (int)(amount * price));
                    }
                    break;
                }
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
            }
        } else {
            if (LOCAL_PLAYER.ship.has_scaffold_kit) {
                g.placing_outpost = true;
            } else {
                set_notice("Buy a scaffold kit at a station first.");
            }
        }
    }
    intent.reset = is_key_pressed(SAPP_KEYCODE_R);
    return intent;
}
