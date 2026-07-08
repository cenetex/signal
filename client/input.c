/*
 * input.c -- Input handling for the Signal Space Miner client.
 *
 * =====================================================================
 * Action-key reference. Several keys are overloaded by context; the
 * precedence order below is load-bearing — keep it in sync with the
 * handlers or the controls become inscrutable. Contexts evaluated in
 * the order listed; first match wins for that key.
 * =====================================================================
 *
 *   [E]   1. Docked → LAUNCH. Always. No overloads.
 *         2. Towing a scaffold undocked → place at nearest slot.
 *         3. Plan mode active → lock outpost (ghost sub-mode) or
 *            place / clear module (real sub-mode).
 *         4. Undocked in dock range → dock.
 *         5. Undocked with targeted module outside dock range → dock
 *            (if DOCK) or toggle inspect pane.
 *
 *   [B]   1. Docked → (no action; plan mode needs undocked).
 *         2. Plan mode active → exit plan mode.
 *         3. Otherwise → enter plan mode.
 *         (Docked station tab changes are [Tab], not [B].)
 *
 *   [F]   TRADE tab → next market page.
 *
 *   [1-5] TRADE tab → buy/sell visible row.
 *   [1-3] CONTRACTS tab → track/select contract row.
 *   [1-9] SHIPYARD tab → order scaffold kit row.
 *   [S]   TRADE tab → sell accepted cargo; CONTRACTS tab → load/unload/proof
 *         selected delivery-credit cargo, or deliver matching contract cargo.
 *   [Space] Undocked outside plan mode → hold tractor; tap to release tow.
 *           Fragments slingshot; cargo pods gently detach for intake handoff.
 *   [R]   SHIP panel → repair; plan mode → cycle module type.
 *   [M]   SHIP panel → upgrade mining laser; undocked → mining laser.
 *   [C]   SHIP panel → upgrade cargo hold.
 *   [H]   Hail ping / contact scan. Undocked hail can reveal contracts, but
 *         cargo pickup requires docking and [S]. Station credits settle when
 *         you dock; there is no separate collect action.
 *   [T]   SHIP panel → upgrade tractor.
 *
 *   [X]   Undocked → self-destruct (hold 1s; single-press no longer
 *         triggers).
 *   [O]   Any → toggle mining autopilot (signal-gated).
 *   [V]   Undocked (strong signal) → hold to talk (pilot mic to NAV-7).
 *   [P] [ ] ]   Music controls (any context).
 *   [Shift]     Undocked → boost.
 *   [Esc]       Plan mode → exit  |  Episode popup → dismiss.
 *               (NOT bound in docked UI — use [Tab] to switch views.)
 *   [Tab]       Docked → cycle station panels (SHIP / TRADE / CONTRACTS / YARD).
 *               Shift+Tab reverses. Undocked scan pane → open/page receipt
 *               relay view; Shift+Tab closes it. No scan pane → scoreboard.
 *               Visibility comes from station panels.
 *
 * If adding a new overloaded key, update this table FIRST so the
 * precedence is visible before the code diverges.
 */
#include <stdarg.h>
#include "input.h"
#include "music.h"
#include "net.h"
#include "net_sync.h"
#include "onboarding.h"
#include "signal_model.h"
#include "mining.h"
#include "manifest.h"
#include "contract_fit.h"
#include "npc_radio.h"
#include "ship.h"

static float action_predict_window_sec(void) {
    float window = 0.5f;
    if (g.net_authority_enabled && g.net_last_ack_rtt > 0.0f) {
        window = 0.25f + g.net_last_ack_rtt * 2.0f;
        if (window < 0.5f) window = 0.5f;
        if (window > 2.0f) window = 2.0f;
    }
    return window;
}

void clear_input_state(void) {
    memset(g.input.key_down, 0, sizeof(g.input.key_down));
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
    g.input.brake_stop_latched = false;
    g.input.reverse_thrust_active = false;
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

static const char *input_station_currency(const station_t *st) {
    if (!st || !st->currency_name[0]) return "cr";
    return st->currency_name;
}

static const char *input_current_currency(void) {
    return input_station_currency(current_station_ptr());
}

static const NetDeliveryLedgerEntry *input_delivery_ledger_for_contract(
    int contract_index)
{
    for (int i = 0; i < g.delivery_ledger_count; i++) {
        const NetDeliveryLedgerEntry *entry = &g.delivery_ledger[i];
        if (entry->contract_index == (uint8_t)contract_index)
            return entry;
    }
    return NULL;
}

static void input_credit_cargo_route_label(const contract_t *ct,
                                           char *out,
                                           size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!ct || ct->action != CONTRACT_DELIVERY ||
        ct->target_index < 0 || ct->target_index >= MAX_STATIONS ||
        ct->station_index >= MAX_STATIONS) {
        snprintf(out, out_size, "cargo");
        return;
    }
    snprintf(out, out_size, "cargo %s>%s",
             station_short_name(ct->target_index),
             station_short_name(ct->station_index));
}

static int input_ship_manifest_count_c(const ship_t *ship, commodity_t commodity) {
    if (!ship || !ship->manifest.units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (u->commodity == (uint8_t)commodity) n++;
    }
    return n;
}

static int input_station_manifest_count_c(const station_t *st, commodity_t commodity) {
    if (!st) return 0;
    int s = (int)(st - g.world.stations);
    if (s < 0 || s >= MAX_STATIONS) return 0;
    if ((int)commodity < 0 || (int)commodity >= COMMODITY_COUNT) return 0;
    int total = 0;
    for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
        total += (int)g.station_manifest_summary[s][commodity][gi];
    return total;
}

static int input_contract_quantity_goal(const contract_t *ct) {
    int qty = (ct && ct->quantity_needed > 0.5f)
            ? (int)ceilf(ct->quantity_needed)
            : 1;
    return qty > 0 ? qty : 1;
}

bool is_key_down(sapp_keycode key) {
    /* Cast to int both sides so gcc -Werror=enum-compare doesn't flag
     * comparing the sokol enum against KEY_COUNT (different anon enum). */
    return ((int)key >= 0) && ((int)key < (int)KEY_COUNT) && g.input.key_down[key];
}

bool is_key_pressed(sapp_keycode key) {
    return ((int)key >= 0) && ((int)key < (int)KEY_COUNT) && g.input.key_pressed[key];
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
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS && count < max; s++) {
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

/* ================================================================== */
/* sample_input_intent — per-concern samplers, each takes the running  */
/* input_intent_t by pointer and mutates the relevant fields. The      */
/* outer function is now an init + ordered call list; CCN drops to a   */
/* short straight-line shape.                                          */
/* ================================================================== */

void input_sample_movement(input_intent_t *intent) {
    const float reverse_start_speed = 2.0f;
    if (LOCAL_PLAYER.docked) {
        g.input.brake_stop_latched = false;
        g.input.reverse_thrust_active = false;
        return;
    }
    bool forward_down = is_key_down(SAPP_KEYCODE_W) || is_key_down(SAPP_KEYCODE_UP);
    bool brake_down = is_key_down(SAPP_KEYCODE_S) || is_key_down(SAPP_KEYCODE_DOWN);
    bool brake_pressed = is_key_pressed(SAPP_KEYCODE_S) || is_key_pressed(SAPP_KEYCODE_DOWN);

    if (is_key_down(SAPP_KEYCODE_A) || is_key_down(SAPP_KEYCODE_LEFT))  intent->turn   += 1.0f;
    if (is_key_down(SAPP_KEYCODE_D) || is_key_down(SAPP_KEYCODE_RIGHT)) intent->turn   -= 1.0f;
    if (forward_down) intent->thrust += 1.0f;

    if (!brake_down || forward_down) {
        g.input.brake_stop_latched = false;
        g.input.reverse_thrust_active = false;
    } else {
        bool stopped = v2_len_sq(LOCAL_PLAYER.ship.vel) <= reverse_start_speed * reverse_start_speed;
        if (brake_pressed) {
            g.input.reverse_thrust_active = stopped;
            g.input.brake_stop_latched = !stopped;
        }
        if (g.input.reverse_thrust_active) {
            intent->thrust -= 1.0f;
            intent->reverse_thrust = true;
        } else if (!(g.input.brake_stop_latched && stopped)) {
            intent->thrust -= 1.0f;
        }
    }

    if (intent->thrust != 0.0f || intent->turn != 0.0f) onboarding_mark_moved();
    intent->mine = is_key_down(SAPP_KEYCODE_M);
}

void input_sample_network_controls(input_intent_t *intent) {
    if (!intent) return;
    input_sample_movement(intent);
    if (!LOCAL_PLAYER.docked && !g.plan_mode_active &&
        is_key_down(SAPP_KEYCODE_SPACE)) {
        intent->tractor_hold = true;
    }
    if (!LOCAL_PLAYER.docked &&
        (is_key_down(SAPP_KEYCODE_LEFT_SHIFT) ||
         is_key_down(SAPP_KEYCODE_RIGHT_SHIFT))) {
        intent->boost = true;
    }
}

bool input_intent_has_network_action(const input_intent_t *intent) {
    if (!intent) return false;
    return intent->interact || intent->service_sell ||
        intent->service_repair || intent->upgrade_mining ||
        intent->upgrade_hold || intent->upgrade_tractor ||
        intent->place_outpost || intent->buy_scaffold_kit ||
        intent->commission_ship || intent->buy_product || intent->hail ||
        intent->release_tow || intent->reset || intent->add_plan ||
        intent->create_planned_outpost || intent->cancel_planned_outpost ||
        intent->cancel_plan_slot || intent->toggle_autopilot;
}

uint8_t input_intent_net_flags(const input_intent_t *intent) {
    uint8_t flags = 0;
    if (!intent) return flags;
    if (intent->thrust > 0.01f)
        flags |= NET_INPUT_THRUST;
    if (intent->thrust < -0.01f)
        flags |= NET_INPUT_BRAKE;
    if (intent->reverse_thrust)
        flags |= NET_INPUT_REVERSE;
    if (intent->turn > 0.01f)
        flags |= NET_INPUT_LEFT;
    if (intent->turn < -0.01f)
        flags |= NET_INPUT_RIGHT;
    if (intent->mine)
        flags |= NET_INPUT_FIRE;
    if (intent->tractor_hold)
        flags |= NET_INPUT_TRACTOR;
    if (intent->boost)
        flags |= NET_INPUT_BOOST;
    return flags;
}

/* Tractor: hold Space = grab, tap Space (< 200ms) = release. */
static void sample_tractor(input_intent_t *intent) {
    if (LOCAL_PLAYER.docked) {
        g.input.tractor_press_time = 0.0f;
        return;
    }
    if (is_key_down(SAPP_KEYCODE_SPACE) && !g.plan_mode_active) {
        if (g.input.tractor_press_time == 0.0f)
            g.input.tractor_press_time = g.world.time;
        intent->tractor_hold = true;
        return;
    }
    if (g.input.tractor_press_time > 0.0f) {
        float held = g.world.time - g.input.tractor_press_time;
        if (held < 0.2f) intent->release_tow = true;
        if (intent->release_tow && LOCAL_PLAYER.ship.towed_count > 0 &&
            !g.onboarding.threw) {
            onboarding_mark_threw();
            set_notice("Throw calibrated. Band line predicts impact; tow another fragment to smelt or fight.");
        }
        g.input.tractor_press_time = 0.0f;
    }
}

static void sample_boost(input_intent_t *intent) {
    intent->boost = (is_key_down(SAPP_KEYCODE_LEFT_SHIFT) ||
                     is_key_down(SAPP_KEYCODE_RIGHT_SHIFT))
                    && !LOCAL_PLAYER.docked;
    if (intent->boost) onboarding_mark_boosted();
}

/* Self-destruct: hold X for 1 s while undocked. The HUD badge driven
 * by self_destruct_hold_time in world_draw is the player's confirm
 * window before the reset fires. */
static void sample_self_destruct(input_intent_t *intent) {
    intent->reset = false;
    if (is_key_down(SAPP_KEYCODE_X) && !LOCAL_PLAYER.docked) {
        if (g.input.self_destruct_hold_time == 0.0f)
            g.input.self_destruct_hold_time = g.world.time;
        if (g.world.time - g.input.self_destruct_hold_time >= 1.0f) {
            intent->reset = true;
            g.input.self_destruct_hold_time = 0.0f;
        }
    } else {
        g.input.self_destruct_hold_time = 0.0f;
    }
}

static void sample_ui_safety(void) {
    /* Clear placement reticle if no longer towing or now docked. */
    if (g.placement_reticle_active &&
        (LOCAL_PLAYER.docked || LOCAL_PLAYER.ship.towed_scaffold < 0)) {
        g.placement_reticle_active = false;
    }
    /* Close inspect pane when docked. */
    if (LOCAL_PLAYER.docked) { g.inspect_station = -1; g.inspect_module = -1; }
}

/* Mining beam auto-targets the nearest module in the beam cone.
 * Targets clear if the laser releases or the player drifts out of range. */
static void sample_targeting(const input_intent_t *intent) {
    if (intent->mine && !LOCAL_PLAYER.docked &&
        LOCAL_PLAYER.in_dock_range && LOCAL_PLAYER.nearby_station >= 0) {
        const station_t *st = station_at(LOCAL_PLAYER.nearby_station);
        if (!st) {
            g.target_station = -1;
            g.target_module = -1;
            return;
        }
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
            if (d > 0.7f && d > best_dot) { best_dot = d; best_mod = idx; }
        }
        if (best_mod >= 0) {
            g.target_station = LOCAL_PLAYER.nearby_station;
            g.target_module = best_mod;
        } else {
            g.target_station = -1;
            g.target_module = -1;
        }
        return;
    }
    /* Laser released: keep target briefly so E can fire it, but clear
     * if the player drifted out of 1.5× tractor range. */
    if (!intent->mine && g.target_station >= 0 && g.target_module >= 0) {
        const station_t *tst = station_at(g.target_station);
        if (!tst || g.target_module >= tst->module_count) {
            g.target_station = -1;
            g.target_module = -1;
            return;
        }
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

/* E key: docked = LAUNCH; scaffold/plan modes are handled by their own
 * samplers; undocked in dock range = dock. Outside dock range, a targeted
 * module can be inspected or used as a dock target. */
static void sample_e_interact(input_intent_t *intent) {
    if (!is_key_pressed(SAPP_KEYCODE_E)) return;
    if (LOCAL_PLAYER.docked) { intent->interact = true; return; }
    if (LOCAL_PLAYER.ship.towed_scaffold >= 0 || g.plan_mode_active) return;
    if (LOCAL_PLAYER.in_dock_range) {
        intent->interact = true;
        g.target_station = -1;
        g.target_module = -1;
        return;
    }
    if (g.target_station >= 0 && g.target_module >= 0) {
        const station_t *tst = station_at(g.target_station);
        if (!tst) {
            g.target_station = -1;
            g.target_module = -1;
            return;
        }
        if (g.target_module < tst->module_count) {
            if (tst->modules[g.target_module].type == MODULE_DOCK) {
                intent->interact = true;
            } else if (g.inspect_station == g.target_station &&
                       g.inspect_module == g.target_module) {
                g.inspect_station = -1;
                g.inspect_module = -1;
            } else {
                g.inspect_station = g.target_station;
                g.inspect_module = g.target_module;
            }
        }
        g.target_station = -1;
        g.target_module = -1;
        return;
    }
    if (LOCAL_PLAYER.in_dock_range) intent->interact = true;
}

/* [Tab] cycles only the panels visible at this station. Shift+Tab reverses. */
static void sample_station_tab(void) {
    if (!LOCAL_PLAYER.docked || !is_key_pressed(SAPP_KEYCODE_TAB)) return;
    bool shift = is_key_down(SAPP_KEYCODE_LEFT_SHIFT) ||
                 is_key_down(SAPP_KEYCODE_RIGHT_SHIFT);
    g.station_view = station_panel_next_visible(
        g.station_view, current_station_ptr(), shift ? -1 : 1);
    g.selected_contract = -1;
}

void station_panel_input_yard(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_YARD) return;
    const station_t *st = current_station_ptr();
    static const struct {
        sapp_keycode key;
        hull_class_t hull;
    } ship_keys[] = {
        { SAPP_KEYCODE_U, HULL_CLASS_DRONE_TRACTOR },
        { SAPP_KEYCODE_I, HULL_CLASS_DRONE_LASER },
        { SAPP_KEYCODE_O, HULL_CLASS_DRONE_CARGO },
        { SAPP_KEYCODE_Z, HULL_CLASS_NPC_MINER },
        { SAPP_KEYCODE_X, HULL_CLASS_HAULER },
        { SAPP_KEYCODE_V, HULL_CLASS_MINER },
    };
    for (size_t i = 0; i < sizeof(ship_keys) / sizeof(ship_keys[0]); i++) {
        if (!is_key_pressed(ship_keys[i].key)) continue;
        if (!station_shipyard_can_commission_hull_local(st, ship_keys[i].hull)) {
            set_notice("Ship commission needs yard stock or hopper-staged pods.");
        } else {
            intent->commission_ship = true;
            intent->commission_hull_class = ship_keys[i].hull;
            set_notice("Commissioned %s.", ship_loadout_name(ship_keys[i].hull));
        }
        return;
    }
    int shown = 0;
    for (int t = 0; t < MODULE_COUNT && shown < 9; t++) {
        module_type_t kit = (module_type_t)t;
        if (module_kind(kit) == MODULE_KIND_NONE) continue;
        if (!station_can_order_scaffold(st, kit)) continue;
        if (!module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules, kit)) continue;
        if (is_key_pressed(SAPP_KEYCODE_1 + shown)) {
            if (st->pending_scaffold_count >= 4) {
                set_notice("Shipyard queue full.");
            } else if ((int)lroundf(player_current_balance()) < scaffold_order_fee(kit)) {
                set_notice("Need %d %s to order.",
                           scaffold_order_fee(kit), input_current_currency());
            } else {
                intent->buy_scaffold_kit = true;
                intent->scaffold_kit_module = kit;
                set_notice("Ordered %s scaffold.", module_type_name(kit));
            }
            return;
        }
        shown++;
    }
}

void station_panel_input_history(input_intent_t *intent) {
    (void)intent;
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_HISTORY) return;

    static const char *names[4] = {
        "all route memory",
        "outbound route memory",
        "inbound route memory",
        "local signed events",
    };
    for (int k = 0; k < 4; k++) {
        if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
        g.history_filter = (uint8_t)k;
        set_notice("History filter: %s.", names[k]);
        return;
    }
}

/* CONTRACTS panel keys:
 *   [1/2/3] select a contract slot for selective delivery
 *   [S]     load/unload/proof delivery credit, or deliver matching cargo
 * The display in station_ui.c sorts deliverable contracts first so [1]
 * usually picks "the contract you can fulfill right now". */
void station_panel_input_work(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_WORK) return;

    const station_t *here_st = current_station_ptr();
    int here_idx = LOCAL_PLAYER.current_station;
    vec2 here_pos = here_st ? here_st->pos : v2(0.0f, 0.0f);
    int slot_contract[3] = {-1, -1, -1};
    bool slot_full[3] = {false, false, false};
    int slot_held_in[3] = {0, 0, 0};
    (void)build_work_slots(here_idx, here_pos, slot_contract, slot_full, slot_held_in);
    (void)slot_full;
    (void)slot_held_in;

    /* [1/2/3] select a contract slot. */
    for (int k = 0; k < 3; k++) {
        if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
        if (slot_contract[k] < 0) break;
        g.selected_contract = slot_contract[k];
        g.tracked_contract = slot_contract[k];
        char step[192];
        if (contract_step_hint(step, sizeof(step)))
            set_notice("%s", step);
        else {
            const contract_t *ct = &g.world.contracts[slot_contract[k]];
            set_notice("Tracking %s contract.",
                       commodity_short_name(ct->commodity));
        }
        break;
    }

    if (!is_key_pressed(SAPP_KEYCODE_S)) return;

    /* [S] acts on the selected contract. If nothing is selected, it
     * sells/delivers everything accepted here. */
    if (g.selected_contract >= 0 && g.selected_contract < MAX_CONTRACTS) {
        const contract_t *ct = &g.world.contracts[g.selected_contract];
        if (ct->active) {
            if (ct->action == CONTRACT_DELIVERY) {
                const NetDeliveryLedgerEntry *ledger =
                    input_delivery_ledger_for_contract(g.selected_contract);
                char cargo_route[48];
                input_credit_cargo_route_label(ct, cargo_route,
                                               sizeof(cargo_route));
                bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
                bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;
                int held = ledger ? (int)ledger->held_bound : 0;
                if (at_origin && ledger &&
                    ledger->status == DELIVERY_SHIPMENT_DELIVERED) {
                    intent->hail = true;
                    set_notice("Returning delivery proof...");
                } else if (at_origin && !ledger) {
                    int source_stock =
                        station_contract_source_stock_count(here_st, ct);
                    if (source_stock <= 0) {
                        set_notice("%s has no %s ready.",
                                   here_st ? here_st->name : "Origin",
                                   cargo_route);
                        return;
                    }
                    intent->hail = true;
                    int qty = input_contract_quantity_goal(ct);
                    if (qty > source_stock) qty = source_stock;
                    set_notice("Loading %s x%d on credit...",
                               cargo_route, qty);
                } else if (at_dest && ledger &&
                           ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
                           held > 0) {
                    intent->service_sell = true;
                    intent->service_sell_only = ct->commodity;
                    set_notice("Unloading %s pod...",
                               cargo_route);
                } else {
                    intent->hail = true;
                    set_notice("Contacting station...");
                }
            } else {
                intent->service_sell = true;
                intent->service_sell_only = ct->commodity;
                if (ct->action == CONTRACT_TRACTOR &&
                    ct->commodity < COMMODITY_RAW_ORE_COUNT) {
                    set_notice("Loading %s...",
                               commodity_short_name(ct->commodity));
                } else if (ct->action == CONTRACT_TRACTOR) {
                    set_notice("Unloading %s pod...",
                               commodity_short_name(ct->commodity));
                } else {
                    set_notice("Delivering %s...",
                               commodity_short_name(ct->commodity));
                }
            }
        } else {
            /* Selected contract was completed/cancelled; fall back. */
            intent->service_sell = true;
            intent->service_sell_only = COMMODITY_COUNT;
            set_notice(LOCAL_PLAYER.ship.towed_pod_count > 0
                ? "Tow cargo crates to matching intakes."
                : "Selling accepted cargo...");
        }
        g.selected_contract = -1;
        return;
    }
    intent->service_sell = true;
    intent->service_sell_only = COMMODITY_COUNT;
    set_notice(LOCAL_PLAYER.ship.towed_pod_count > 0
        ? "Tow cargo crates to matching intakes."
        : "Selling accepted cargo...");
}

/* SHIP panel keys:
 *   [R] REPAIR
 *   [M] upgrade mining laser
 *   [C] upgrade cargo hold capacity
 *   [T] upgrade tractor */
void station_panel_input_dock(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_DOCK) return;
    if (is_key_pressed(SAPP_KEYCODE_R)) {
        const station_t *st = current_station_ptr();
        int kits_avail =
            input_ship_manifest_count_c(&LOCAL_PLAYER.ship, COMMODITY_REPAIR_KIT) +
            input_station_manifest_count_c(st, COMMODITY_REPAIR_KIT);
        float max_hull = ship_max_hull(&LOCAL_PLAYER.ship);
        bool needs_repair = LOCAL_PLAYER.ship.hull < max_hull;
        if (needs_repair && kits_avail <= 0) {
            int hp_needed = (int)ceilf(max_hull - LOCAL_PLAYER.ship.hull);
            if (hp_needed < 1) hp_needed = 1;
            set_notice("%d repair kit%s needed.",
                       hp_needed, hp_needed == 1 ? "" : "s");
        } else intent->service_repair = true;
    }
    intent->upgrade_mining  = is_key_pressed(SAPP_KEYCODE_M);
    intent->upgrade_hold    = is_key_pressed(SAPP_KEYCODE_C);
    intent->upgrade_tractor = is_key_pressed(SAPP_KEYCODE_T);
}

/* TRADE tab [S] — sell the next accepted pod, then legacy cargo fallback. */
static void sample_trade_sell_all(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_TRADE) return;
    if (!is_key_pressed(SAPP_KEYCODE_S)) return;
    intent->service_sell = true;
    intent->service_sell_only = COMMODITY_COUNT;
    set_notice(LOCAL_PLAYER.ship.towed_pod_count > 0
        ? "Tow cargo crates to matching intakes."
        : "Selling accepted cargo...");
}

/* TRADE picker — page through the unified row list and dispatch on the
 * digit pick. Row construction lives in station_ui.c:build_trade_rows
 * so the renderer and the input handler share a single source of
 * truth. Mismatched [1] hotkeys aren't possible by construction. */
static void trade_apply_buy_row(input_intent_t *intent, const station_t *st,
                                 const ship_t *ship, const trade_row_t *row) {
    int quantity = row->quantity > 0 ? row->quantity : 1;
    float total_price = (float)(row->total_price > 0
        ? row->total_price : row->unit_price * quantity);
    int tow_space = ship_tow_body_space(ship);
    float balance = player_current_balance();
    if (tow_space <= 0) {
        set_notice("Tow slots full.");
        return;
    }
    if (balance + FLOAT_EPSILON < total_price) {
        set_notice("Need %d %s.",
                   (int)lroundf(total_price), input_station_currency(st));
        return;
    }

    intent->buy_product = true;
    intent->buy_commodity = row->commodity;
    intent->buy_grade = row->grade;
    if (row->is_station_pod) {
        intent->buy_station_pod = true;
        intent->buy_station_pod_index = row->station_pod_index;
    }
    if (row->is_station_pod) {
        set_notice("-%d %s  %s crate x%d",
                   (int)lroundf(total_price),
                   input_station_currency(st),
                   commodity_short_name(row->commodity), quantity);
    } else {
        set_notice("-%d %s  %s %s x%d",
                   (int)lroundf(total_price),
                   input_station_currency(st),
                   mining_grade_label(row->grade),
                   commodity_short_name(row->commodity), quantity);
    }
    (void)st;
}

static void sample_trade_picker(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_TRADE) return;
    const station_t *st = current_station_ptr();
    if (is_key_pressed(SAPP_KEYCODE_F)) g.trade_page++;
    int digit_pick = -1;
    for (int i = 0; i < 5 && digit_pick < 0; i++)
        if (is_key_pressed(SAPP_KEYCODE_1 + i)) digit_pick = i;
    if (digit_pick < 0 || !st) return;

    const ship_t *ship = &LOCAL_PLAYER.ship;
    trade_row_t rows[TRADE_MAX_ROWS];
    int row_count = build_trade_rows(st, ship, rows, TRADE_MAX_ROWS);
    int page_first = 0, page_last = 0, total_pages = 1;
    trade_page_range(rows, row_count, (int)g.trade_page,
                     &page_first, &page_last, &total_pages);
    if ((int)g.trade_page >= total_pages) g.trade_page = 0;

    /* Hotkey = row position on page (digit_pick is 0-based). The renderer
     * uses the same mapping; a blocked row holds its slot but we no-op
     * here so numbers stay locked for the player's muscle memory. */
    int target = page_first + digit_pick;
    if (target < 0 || target >= page_last || !rows[target].actionable) return;
    const trade_row_t *row = &rows[target];
    if (row->kind == 0) {
        trade_apply_buy_row(intent, st, ship, row);
    } else if (row->kind == 1) {
        /* Per-row sell — mirror of the buy click. One press sells one
         * matching towed pod. Legacy manifest-held cargo is no longer
         * an authoritative sell surface. */
        if (row->held <= 0) {
            set_notice("Out of %s.", commodity_short_name(row->commodity));
            return;
        }
        if (!row->is_towed_pod) {
            set_notice("Cargo must be in a pod.");
            return;
        }
        intent->service_sell = true;
        intent->service_sell_only = row->commodity;
        intent->service_sell_grade = MINING_GRADE_COUNT;
        intent->service_sell_one = false;
        float price = (float)(row->total_price > 0
            ? row->total_price
            : row->unit_price);
        float payout = price;
        if (row->is_towed_pod) {
            set_notice("+%d %s  %s crate x%d",
                       (int)lroundf(payout),
                       input_station_currency(st),
                       commodity_short_name(row->commodity),
                       row->towed_pod_quantity > 0 ? row->towed_pod_quantity
                                                   : row->quantity);
        } else {
            set_notice("+%d %s  %s %s",
                       (int)lroundf(payout),
                       input_station_currency(st),
                       mining_grade_label(row->grade),
                       commodity_short_name(row->commodity));
        }
    }
}

void station_panel_input_trade(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_TRADE) return;
    sample_trade_sell_all(intent);
    sample_trade_picker(intent);
}

/* Tow mode: server snaps to the closest slot on E. */
static void sample_placement_tow(input_intent_t *intent) {
    g.placement_reticle_active = false;
    if (!is_key_pressed(SAPP_KEYCODE_E)) return;
    reticle_target_t targets[RETICLE_MAX_TARGETS];
    int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
    intent->place_outpost = true;
    const char *scaffold_name = "scaffold";
    int sc_idx = LOCAL_PLAYER.ship.towed_scaffold;
    if (sc_idx >= 0 && sc_idx < MAX_SCAFFOLDS && g.world.scaffolds[sc_idx].active)
        scaffold_name = module_type_name(g.world.scaffolds[sc_idx].module_type);
    if (n > 0) {
        intent->place_target_station = (int8_t)targets[0].station;
        intent->place_target_ring = (int8_t)targets[0].ring;
        intent->place_target_slot = (int8_t)targets[0].slot;
        set_notice("Placing %s at ring %d slot %d.",
                   scaffold_name, targets[0].ring, targets[0].slot);
    } else {
        set_notice("Placing %s as an outpost seed.", scaffold_name);
    }
    /* No outpost in range falls through with all -1 sentinels — server
     * decides whether to materialize a nearby planned station or found
     * a new outpost from scratch. */
}

/* Plan mode (real station target): pull reticle target every frame so
 * the rings track the player's nearest slot. If nothing in range for
 * grace_until expiry, exit plan mode. */
static void plan_mode_real_track(void) {
    reticle_target_t targets[RETICLE_MAX_TARGETS];
    int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
    if (n == 0) {
        if (g.world.time >= g.plan_mode_grace_until) g.plan_mode_active = false;
        return;
    }
    g.placement_target_station = targets[0].station;
    g.placement_target_ring = targets[0].ring;
    g.placement_target_slot = targets[0].slot;
    g.plan_mode_grace_until = 0.0f;
}

/* Plan mode (ghost preview): pick the slot closest to the ship's
 * forward direction. Ghost preview rings draw around the player's
 * ship, no server message until E. */
static void plan_mode_ghost_track(void) {
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

/* Plan mode B/Esc exit. Returns true if exit fired. Cancel a real
 * planned outpost too if it was empty (no plans yet). */
static bool plan_mode_handle_exit(input_intent_t *intent, bool ghost_mode) {
    if (!is_key_pressed(SAPP_KEYCODE_ESCAPE) && !is_key_pressed(SAPP_KEYCODE_B))
        return false;
    if (!ghost_mode) {
        int s = g.placement_target_station;
        if (s >= SIGNAL_FIRST_OUTPOST_INDEX && s < MAX_STATIONS &&
            g.world.stations[s].planned &&
            g.world.stations[s].placement_plan_count == 0) {
            intent->cancel_planned_outpost = true;
            intent->cancel_planned_station = (int8_t)s;
            set_notice("Outpost design cancelled.");
        }
    }
    g.plan_mode_active = false;
    return true;
}

/* Plan mode R: cycle through unlocked, available module types. */
static void plan_mode_handle_cycle_type(input_intent_t *intent) {
    if (!is_key_pressed(SAPP_KEYCODE_R)) return;
    static const module_type_t plannable[] = {
        MODULE_FURNACE,
        MODULE_FRAME_PRESS, MODULE_LASER_FAB, MODULE_TRACTOR_FAB,
        /* MODULE_ORE_SILO + MODULE_CARGO_BAY were dropped — HOPPER
         * absorbs the storage role. */
        MODULE_HOPPER,
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
    intent->release_tow = false;
}

/* Plan mode E in ghost preview mode: lock the outpost. */
static void plan_mode_handle_ghost_lock(input_intent_t *intent) {
    vec2 pos = LOCAL_PLAYER.ship.pos;
    bool too_close = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        if (v2_dist_sq(st->pos, pos) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) {
            too_close = true; break;
        }
    }
    float here_sig = signal_strength_at(&g.world, pos);
    if (too_close) { set_notice("Too close to an existing station."); return; }
    if (here_sig <= 0.0f) { set_notice("No signal here."); return; }
    if (here_sig >= OUTPOST_MAX_SIGNAL) {
        set_notice("Too deep in station coverage. Move to the fringe.");
        return;
    }
    /* Atomic create + first plan */
    intent->create_planned_outpost = true;
    intent->planned_outpost_pos = pos;
    intent->add_plan = true;
    intent->plan_station = -2; /* sentinel: just-created */
    intent->plan_ring = (int8_t)g.placement_target_ring;
    intent->plan_slot = (int8_t)g.placement_target_slot;
    intent->plan_type = (module_type_t)g.plan_type;
    g.outpost_lock_timer = 1.5f;
    g.outpost_lock_pos = pos;
    /* Wait for the server to send back the created station, then switch
     * to real plan mode targeting it. */
    g.plan_mode_grace_until = g.world.time + 1.5f;
    set_notice("Outpost blueprint locked: %s ring %d slot %d. R changes type; E toggles slot; B exits.",
               module_type_name((module_type_t)g.plan_type),
               g.placement_target_ring, g.placement_target_slot);
}

/* Plan mode E on a real station: toggle the slot's plan. */
static void plan_mode_handle_real_place(input_intent_t *intent) {
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
        intent->cancel_plan_slot = true;
        intent->cancel_plan_st = (int8_t)ps;
        intent->cancel_plan_ring = (int8_t)pr;
        intent->cancel_plan_sl = (int8_t)psl;
        set_notice("Cleared reserved slot ring %d slot %d. R changes type; E toggles slot; B exits.",
                   pr, psl);
        return;
    }
    intent->add_plan = true;
    intent->plan_station = (int8_t)ps;
    intent->plan_ring = (int8_t)pr;
    intent->plan_slot = (int8_t)psl;
    intent->plan_type = (module_type_t)g.plan_type;
    set_notice("Reserved %s at ring %d slot %d. R changes type; E toggles slot; B exits.",
               module_type_name((module_type_t)g.plan_type), pr, psl);
}

/* Plan mode E dispatch: lock, type-cycle, or place. */
static void plan_mode_handle_e(input_intent_t *intent, bool ghost_mode) {
    if (!is_key_pressed(SAPP_KEYCODE_E)) return;
    module_type_t planned[PLAYER_PLAN_TYPE_LIMIT];
    int planned_n = player_planned_types(planned, PLAYER_PLAN_TYPE_LIMIT);
    bool already = false;
    for (int k = 0; k < planned_n; k++)
        if (planned[k] == (module_type_t)g.plan_type) { already = true; break; }
    if (!module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules,
                                    (module_type_t)g.plan_type)) {
        set_notice("%s is locked.", module_type_name((module_type_t)g.plan_type));
        return;
    }
    if (!already && planned_n >= PLAYER_PLAN_TYPE_LIMIT) {
        set_notice("You can plan %d module types. Clear one first.", PLAYER_PLAN_TYPE_LIMIT);
        return;
    }
    if (ghost_mode) plan_mode_handle_ghost_lock(intent);
    else            plan_mode_handle_real_place(intent);
}

/* Plan mode top-level: track target, then dispatch B/Esc/R/E. */
static void sample_plan_mode(input_intent_t *intent) {
    bool ghost_mode = (g.plan_target_station == -1);
    if (!ghost_mode) plan_mode_real_track();
    else             plan_mode_ghost_track();
    if (plan_mode_handle_exit(intent, ghost_mode)) return;
    if (!g.plan_mode_active) return;
    plan_mode_handle_cycle_type(intent);
    plan_mode_handle_e(intent, ghost_mode);
}

/* B undocked, not towing: enter plan mode targeting nearest outpost
 * (real) or kick off ghost preview (none in range). */
static void sample_b_enter_plan(void) {
    if (!is_key_pressed(SAPP_KEYCODE_B) || LOCAL_PLAYER.docked) return;
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
        set_notice("Station plan: %s ring %d slot %d. R changes type; E toggles slot; B exits.",
                   module_type_name((module_type_t)g.plan_type),
                   targets[0].ring, targets[0].slot);
    } else {
        g.plan_mode_active = true;
        g.plan_target_station = -1; /* sentinel: ghost */
        g.placement_target_station = -1;
        g.placement_target_ring = 1;
        g.placement_target_slot = 0;
        set_notice("Ghost preview: %s ring %d slot %d. R changes type; E locks outpost; B exits.",
                   module_type_name((module_type_t)g.plan_type),
                   g.placement_target_ring, g.placement_target_slot);
    }
}

/* B / R / E: placement (tow mode), planning (plan mode), or enter-plan. */
static void sample_placement(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked && LOCAL_PLAYER.ship.towed_scaffold >= 0) {
        sample_placement_tow(intent);
        return;
    }
    if (g.plan_mode_active) {
        sample_plan_mode(intent);
        return;
    }
    sample_b_enter_plan();
}

/* [ ] keys cycle music tracks; / toggles pause. */
static void sample_music(void) {
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
    if (is_key_pressed(SAPP_KEYCODE_SLASH)) {
        if (g.music.paused) {
            music_resume(&g.music);
            set_notice("MUSIC RESUMED");
        } else {
            music_pause(&g.music);
            set_notice("MUSIC PAUSED");
        }
    }
}

/* H: hail/scan ping. Visual ring fires locally so the press feels instant;
 * server decides which station or local objects respond. */
static void sample_hail(input_intent_t *intent) {
    if (!is_key_pressed(SAPP_KEYCODE_H)) return;
    intent->hail = true;
    g.hail_ping_timer  = 0.001f; /* any nonzero = active */
    g.hail_ping_origin = LOCAL_PLAYER.ship.pos;
    g.hail_ping_range  = (LOCAL_PLAYER.ship.comm_range > 0.0f)
                         ? LOCAL_PLAYER.ship.comm_range : 1500.0f;
    g.hail_conversation_count = npc_radio_build_hail_conversation(
        g.world.stations, g.world.npc_ships,
        g.hail_ping_origin, g.hail_ping_range,
        g.hail_conversation);
    if (!npc_radio_player_line(g.hail_player_line,
                               sizeof(g.hail_player_line))) {
        g.hail_player_line[0] = '\0';
    }
    g.hail_choice_request_id++;
    if (g.hail_choice_request_id == 0) g.hail_choice_request_id = 1;
    size_t prompt_len = npc_radio_build_choice_prompt_for_hail(
        g.world.stations, g.world.npc_ships,
        g.hail_conversation, g.hail_conversation_count,
        g.hail_choice_request_id,
        g.hail_choice_prompt, sizeof(g.hail_choice_prompt));
    g.hail_choice_prompt_len = prompt_len < sizeof(g.hail_choice_prompt)
        ? (uint16_t)prompt_len
        : (uint16_t)(sizeof(g.hail_choice_prompt) - 1);
    g.hail_choice_applied_count = 0;
}

/* O: toggle mining autopilot. Server-side AI runs the mining loop on
 * the player's ship. Manual movement / mine input cancels it. Works
 * docked or undocked. */
static void sample_autopilot(input_intent_t *intent) {
    if (!is_key_pressed(SAPP_KEYCODE_O)) return;
    if (LOCAL_PLAYER.autopilot_mode) {
        intent->toggle_autopilot = true; /* always allow turning off */
        return;
    }
    float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
    if (sig < SIGNAL_BAND_OPERATIONAL) {
        set_notice("Signal too weak for autopilot.");
        return;
    }
    intent->toggle_autopilot = true;
}

/* sample_voice_mic removed with the voice subsystem. */

input_intent_t sample_input_intent(void) {
    input_intent_t intent = { 0 };
    /* Default buy_grade to "any" (sentinel = MINING_GRADE_COUNT) so
     * manifest-first transfers don't accidentally prefer COMMON just
     * because the zero-init lands there. */
    intent.buy_grade = MINING_GRADE_COUNT;
    intent.place_target_station = -1;
    intent.place_target_ring = -1;
    intent.place_target_slot = -1;
    intent.plan_station = -1;
    intent.plan_ring = -1;
    intent.plan_slot = -1;
    intent.cancel_planned_station = -1;
    intent.service_sell_only = COMMODITY_COUNT; /* default: dock-selected service */
    intent.service_sell_grade = MINING_GRADE_COUNT; /* default: any grade */
    intent.service_sell_one = false;

    input_sample_movement(&intent);
    sample_tractor(&intent);
    sample_boost(&intent);
    sample_self_destruct(&intent);
    sample_ui_safety();
    sample_targeting(&intent);
    sample_e_interact(&intent);
    sample_station_tab();
    station_panel_sample_current(&intent);
    sample_placement(&intent);
    sample_music();
    sample_hail(&intent);
    sample_autopilot(&intent);
    return intent;
}

void submit_input(const input_intent_t *intent, float dt) {
    /* Set on client world for prediction */
    LOCAL_PLAYER.input = *intent;

    /* Client prediction is stable only when the server snapshots and local
     * frames share the input-tick clock. Pre-tick remote servers remain
     * server-authoritative while the client still sends inputs normally. */
    if (net_local_prediction_enabled() && !LOCAL_PLAYER.docked) {
        net_replay_record_prediction(intent, dt);
        world_sim_step_player_only(&g.world, g.local_player_slot, dt);
        net_adopt_local_tow_prediction(dt);
    }

    /* Detect one-shot actions for prediction suppression and network send */
    bool has_action = input_intent_has_network_action(intent);

    if (has_action)
        g.action_predict_timer = action_predict_window_sec();

    /* Networked authority: plan intents ride a dedicated message — they
     * carry richer payloads (target station/ring/slot/type or world
     * position) that don't fit in the 1-byte action slot. */
    if (g.net_authority_enabled && net_is_connected()) {
        bool plan_send_failed = false;
        if (intent->create_planned_outpost && intent->add_plan &&
            intent->plan_station == -2) {
            /* Atomic create + first plan — single message. */
            if (!net_send_plan(NET_PLAN_OP_CREATE_AND_ADD,
                               -1,
                               intent->plan_ring,
                               intent->plan_slot,
                               (uint8_t)intent->plan_type,
                               intent->planned_outpost_pos.x,
                               intent->planned_outpost_pos.y))
                plan_send_failed = true;
        } else {
            if (intent->create_planned_outpost) {
                if (!net_send_plan(NET_PLAN_OP_CREATE_OUTPOST,
                                   -1, -1, -1, 0,
                                   intent->planned_outpost_pos.x,
                                   intent->planned_outpost_pos.y))
                    plan_send_failed = true;
            }
            if (intent->add_plan) {
                if (!net_send_plan(NET_PLAN_OP_ADD_SLOT,
                                   intent->plan_station,
                                   intent->plan_ring,
                                   intent->plan_slot,
                                   (uint8_t)intent->plan_type,
                                   0.0f, 0.0f))
                    plan_send_failed = true;
            }
        }
        if (intent->cancel_planned_outpost) {
            if (!net_send_plan(NET_PLAN_OP_CANCEL_OUTPOST,
                               intent->cancel_planned_station,
                               -1, -1, 0,
                               0.0f, 0.0f))
                plan_send_failed = true;
        }
        if (intent->cancel_plan_slot) {
            if (!net_send_plan(NET_PLAN_OP_CANCEL_PLAN_SLOT,
                               intent->cancel_plan_st,
                               intent->cancel_plan_ring,
                               intent->cancel_plan_sl,
                               0, 0.0f, 0.0f))
                plan_send_failed = true;
        }
        if (plan_send_failed) {
            set_notice("Unable to submit signed planning action.");
        }
    }

    /* Networked authority: encode the action and queue for send. */
    if (has_action && g.net_authority_enabled && net_is_connected()) {
        if (intent->interact) {
            g.pending_net_action = LOCAL_PLAYER.docked
                ? NET_ACTION_LAUNCH
                : NET_ACTION_DOCK;
        } else if (intent->service_sell && intent->service_sell_only < COMMODITY_COUNT) {
            g.pending_net_action = NET_ACTION_DELIVER_COMMODITY + (uint8_t)intent->service_sell_only;
            /* Per-row sell rides the same 5th-byte slot as buy_grade.
             * Server treats `grade < MINING_GRADE_COUNT` as the selective
             * row signal: one whole pod for pod cargo, one cargo_unit for
             * legacy manifest cargo. */
            if (intent->service_sell_one && intent->service_sell_grade < MINING_GRADE_COUNT)
                g.pending_net_buy_grade = (uint8_t)intent->service_sell_grade;
        }
        else if (intent->service_sell)
            g.pending_net_action = NET_ACTION_SELL_CARGO;
        else if (intent->service_repair)
            g.pending_net_action = NET_ACTION_REPAIR;
        else if (intent->upgrade_mining)
            g.pending_net_action = NET_ACTION_UPGRADE_MINING;
        else if (intent->upgrade_hold)
            g.pending_net_action = NET_ACTION_UPGRADE_HOLD;
        else if (intent->upgrade_tractor)
            g.pending_net_action = NET_ACTION_UPGRADE_TRACTOR;
        else if (intent->place_outpost) {
            g.pending_net_action = NET_ACTION_PLACE_OUTPOST;
            g.pending_net_place_station = intent->place_target_station;
            g.pending_net_place_ring    = intent->place_target_ring;
            g.pending_net_place_slot    = intent->place_target_slot;
        }
        else if (intent->buy_scaffold_kit && (uint8_t)intent->scaffold_kit_module < MODULE_COUNT)
            g.pending_net_action = NET_ACTION_BUY_SCAFFOLD_TYPED + (uint8_t)intent->scaffold_kit_module;
        else if (intent->commission_ship && (uint8_t)intent->commission_hull_class < HULL_CLASS_COUNT)
            g.pending_net_action = NET_ACTION_COMMISSION_SHIP + (uint8_t)intent->commission_hull_class;
        else if (intent->buy_product && (uint8_t)intent->buy_commodity < COMMODITY_COUNT) {
            g.pending_net_action = NET_ACTION_BUY_PRODUCT + (uint8_t)intent->buy_commodity;
            g.pending_net_buy_grade = (uint8_t)intent->buy_grade;
        }
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
