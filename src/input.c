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
 *         4. Undocked with targeted module → dock (if DOCK) or toggle
 *            inspect pane.
 *
 *   [B]   1. Docked → (no action; plan mode needs undocked).
 *         2. Plan mode active → exit plan mode.
 *         3. Otherwise → enter plan mode.
 *         (Note: named-ingot buy lives on [G], NOT [B], precisely to
 *         avoid teaching the player that [B] means 'buy'.)
 *
 *   [G]   Docked → buy first named ingot from HIGH-GRADE STOCK.
 *   [N]   Docked → deliver first named ingot from YOUR HOLD.
 *   [F]   Docked → buy primary product (float commodity).
 *
 *   [1]   STATUS/MARKET → sell cargo  |  CONTRACTS → select contract 1
 *         SHIPYARD → order scaffold 1.
 *   [2]   STATUS → repair  |  CONTRACTS/SHIPYARD → select/order 2.
 *   [3-5] STATUS → upgrade laser/hold/tractor  |  sel/order 3-5.
 *   [6-9] SHIPYARD → order scaffold 6-9 (no STATUS/MARKET meaning).
 *
 *   [X]   Undocked → self-destruct (hold 1s; single-press no longer
 *         triggers).
 *   [H]   Undocked → hail ping + collect pending credits.
 *   [O]   Any → toggle mining autopilot (signal-gated).
 *   [V]   Undocked (strong signal) → hold to talk (pilot mic to NAV-7).
 *   [R]   Plan mode → cycle module type. Outside plan mode → held-tow
 *         (tractor R).
 *   [M]   Undocked → mining laser.
 *   [P] [ ] ]   Music controls (any context).
 *   [Shift]     Undocked → boost.
 *   [Esc]       Plan mode → exit  |  Episode popup → dismiss.
 *               (NOT bound in docked UI — use [Tab] to switch views.)
 *   [Tab]       Docked → cycle station tabs (VERBS / JOBS / BUILD).
 *               Shift+Tab reverses. BUILD skipped at docks with no shipyard.
 *
 * If adding a new overloaded key, update this table FIRST so the
 * precedence is visible before the code diverges.
 */
#include <stdarg.h>
#include "input.h"
#include "local_server.h"
#include "music.h"
#include "net.h"
#include "onboarding.h"
#include "signal_model.h"
#include "mining.h"

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

/* ================================================================== */
/* sample_input_intent — per-concern samplers, each takes the running  */
/* input_intent_t by pointer and mutates the relevant fields. The      */
/* outer function is now an init + ordered call list; CCN drops to a   */
/* short straight-line shape.                                          */
/* ================================================================== */

static void sample_movement(input_intent_t *intent) {
    if (is_key_down(SAPP_KEYCODE_A) || is_key_down(SAPP_KEYCODE_LEFT))  intent->turn   += 1.0f;
    if (is_key_down(SAPP_KEYCODE_D) || is_key_down(SAPP_KEYCODE_RIGHT)) intent->turn   -= 1.0f;
    if (is_key_down(SAPP_KEYCODE_W) || is_key_down(SAPP_KEYCODE_UP))    intent->thrust += 1.0f;
    if (is_key_down(SAPP_KEYCODE_S) || is_key_down(SAPP_KEYCODE_DOWN))  intent->thrust -= 1.0f;
    if (intent->thrust != 0.0f || intent->turn != 0.0f) onboarding_mark_moved();
    intent->mine = is_key_down(SAPP_KEYCODE_M);
}

/* Tractor: hold Space = grab, tap Space (< 200ms) = release. */
static void sample_tractor(input_intent_t *intent) {
    if (is_key_down(SAPP_KEYCODE_SPACE) && !g.plan_mode_active) {
        if (g.input.tractor_press_time == 0.0f)
            g.input.tractor_press_time = g.world.time;
        intent->tractor_hold = true;
        return;
    }
    if (g.input.tractor_press_time > 0.0f) {
        float held = g.world.time - g.input.tractor_press_time;
        if (held < 0.2f) intent->release_tow = true;
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

/* SPACE (laser) auto-targets the nearest module in the beam cone.
 * Targets clear if the laser releases or the player drifts out of range. */
static void sample_targeting(const input_intent_t *intent) {
    if (intent->mine && !LOCAL_PLAYER.docked &&
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

/* E key: docked = LAUNCH; undocked = activate the targeted module
 * (dock if it's a DOCK module, otherwise toggle the inspect pane), or
 * fall back to "dock" when in dock range with no target. */
static void sample_e_interact(input_intent_t *intent) {
    if (!is_key_pressed(SAPP_KEYCODE_E)) return;
    if (LOCAL_PLAYER.docked) { intent->interact = true; return; }
    if (g.target_station >= 0 && g.target_module >= 0) {
        const station_t *tst = &g.world.stations[g.target_station];
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

/* [Tab] cycle docked tabs (DOCK ↔ CONTRACTS ↔ ...). Shift+Tab reverses. */
static void sample_station_tab(void) {
    if (!LOCAL_PLAYER.docked || !is_key_pressed(SAPP_KEYCODE_TAB)) return;
    bool shift = is_key_down(SAPP_KEYCODE_LEFT_SHIFT) ||
                 is_key_down(SAPP_KEYCODE_RIGHT_SHIFT);
    int n = (int)STATION_VIEW_COUNT;
    g.station_view = (station_view_t)(((int)g.station_view +
                                       (shift ? n - 1 : 1)) % n);
    g.selected_contract = -1;
}

/* YARD tab keys: [1-9] order a scaffold kit. Surface every unlocked
 * module type this yard can fabricate. (Plans are still useful for
 * slot reservation, but no longer required to *order* a kit — the
 * chicken-and-egg for the very first SIGNAL_RELAY would be unsolvable.) */
static void sample_yard_keys(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_YARD) return;
    const station_t *st = current_station_ptr();
    int shown = 0;
    for (int t = 0; t < MODULE_COUNT && shown < 9; t++) {
        module_type_t kit = (module_type_t)t;
        if (module_kind(kit) == MODULE_KIND_NONE) continue;
        if (!station_has_module(st, kit)) continue;
        if (!module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules, kit)) continue;
        if (is_key_pressed(SAPP_KEYCODE_1 + shown)) {
            if (st->pending_scaffold_count >= 4) {
                set_notice("Shipyard queue full.");
            } else if ((int)lroundf(player_current_balance()) < scaffold_order_fee(kit)) {
                set_notice("Need %d cr to order.", scaffold_order_fee(kit));
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

/* WORK (JOBS) tab keys:
 *   [1/2/3] select a contract slot for selective delivery
 *   [S]     deliver — selective if a slot is selected, else all
 * The display in station_ui.c sorts deliverable contracts first so [1]
 * usually picks "the contract you can fulfill right now". */
static void sample_work_keys(input_intent_t *intent) {
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
        const contract_t *ct = &g.world.contracts[slot_contract[k]];
        set_notice("Selected: %s. [S] deliver.",
                   commodity_short_name(ct->commodity));
        break;
    }

    if (!is_key_pressed(SAPP_KEYCODE_S)) return;

    /* [S] deliver. Selective if a slot is selected, else everything. */
    if (g.selected_contract >= 0 && g.selected_contract < MAX_CONTRACTS) {
        const contract_t *ct = &g.world.contracts[g.selected_contract];
        if (ct->active) {
            intent->service_sell = true;
            intent->service_sell_only = ct->commodity;
            set_notice("Delivering %s...", commodity_short_name(ct->commodity));
        } else {
            /* Selected contract was completed/cancelled; fall back. */
            intent->service_sell = true;
            intent->service_sell_only = COMMODITY_COUNT;
            set_notice("Delivering all matching cargo...");
        }
        g.selected_contract = -1;
        return;
    }
    intent->service_sell = true;
    intent->service_sell_only = COMMODITY_COUNT;
    set_notice("Delivering all matching cargo...");
}

/* DOCK (ship bay) tab keys:
 *   [R] REPAIR
 *   [M] upgrade mining laser
 *   [H] upgrade hold capacity
 *   [T] upgrade tractor */
static void sample_dock_keys(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_DOCK) return;
    if (is_key_pressed(SAPP_KEYCODE_R)) {
        const station_t *st = current_station_ptr();
        int kits_avail =
            (int)floorf(LOCAL_PLAYER.ship.cargo[COMMODITY_REPAIR_KIT] + 0.0001f) +
            (st ? (int)floorf(st->_inventory_cache[COMMODITY_REPAIR_KIT] + 0.0001f) : 0);
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
    intent->upgrade_hold    = is_key_pressed(SAPP_KEYCODE_H);
    intent->upgrade_tractor = is_key_pressed(SAPP_KEYCODE_T);
}

/* TRADE tab [S] — sell every commodity this station accepts. */
static void sample_trade_sell_all(input_intent_t *intent) {
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_TRADE) return;
    if (!is_key_pressed(SAPP_KEYCODE_S)) return;
    intent->service_sell = true;
    intent->service_sell_only = COMMODITY_COUNT;
    set_notice("Selling...");
}

/* TRADE picker — page through the unified row list and dispatch on the
 * digit pick. Row construction lives in station_ui.c:build_trade_rows
 * so the renderer and the input handler share a single source of
 * truth. Mismatched [1] hotkeys aren't possible by construction. */
static void trade_apply_buy_row(input_intent_t *intent, const station_t *st,
                                 const ship_t *ship, const trade_row_t *row) {
    float price = (float)row->unit_price;
    float free_volume = ship_cargo_capacity(ship) - ship_total_cargo(ship);
    float vol = commodity_volume(row->commodity);
    /* Match server: dense goods buy multi-per-press so a single keystroke
     * fills one cargo unit. */
    int per_press = (vol > FLOAT_EPSILON) ? (int)lroundf(1.0f / vol) : 1;
    if (per_press < 1) per_press = 1;
    if (free_volume + FLOAT_EPSILON < vol) { set_notice("Hold full."); return; }
    if (player_current_balance() < price) { set_notice("Need $%d.", (int)lroundf(price)); return; }

    intent->buy_product = true;
    intent->buy_commodity = row->commodity;
    intent->buy_grade = row->grade;
    LOCAL_PLAYER.ship.cargo[row->commodity] += (float)per_press;
    if (!g.multiplayer_enabled) {
        station_t *mst = &g.world.stations[LOCAL_PLAYER.current_station];
        float total = price * (float)per_press;
        for (int li = 0; li < mst->ledger_count; li++) {
            if (mst->ledger[li].balance >= total) {
                mst->ledger[li].balance -= total;
                break;
            }
        }
    }
    set_notice("-$%d  %s %s x%d",
               (int)lroundf(price * (float)per_press),
               mining_grade_label(row->grade),
               commodity_short_name(row->commodity), per_press);
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
        /* Per-row sell — mirror of the buy click. One press = one unit
         * of (commodity, grade). The bulk [S] hotkey still drains the
         * hold, but clicking a specific row only nibbles the matching
         * cargo_unit so the rest stays on board. */
        if (LOCAL_PLAYER.ship.cargo[row->commodity] < 0.999f) {
            set_notice("Out of %s.", commodity_short_name(row->commodity));
            return;
        }
        intent->service_sell = true;
        intent->service_sell_only = row->commodity;
        intent->service_sell_grade = row->grade;
        intent->service_sell_one = true;
        float price = (float)row->unit_price;
        float bonus_mult = mining_payout_multiplier(row->grade);
        float payout = price * bonus_mult;
        LOCAL_PLAYER.ship.cargo[row->commodity] -= 1.0f;
        if (LOCAL_PLAYER.ship.cargo[row->commodity] < 0.0f)
            LOCAL_PLAYER.ship.cargo[row->commodity] = 0.0f;
        if (!g.multiplayer_enabled) {
            station_t *mst = &g.world.stations[LOCAL_PLAYER.current_station];
            int idx = -1;
            for (int li = 0; li < mst->ledger_count; li++) {
                if (memcmp(mst->ledger[li].player_token,
                           LOCAL_PLAYER.session_token, 8) == 0) { idx = li; break; }
            }
            if (idx < 0 && mst->ledger_count < 16) {
                idx = mst->ledger_count++;
                memcpy(mst->ledger[idx].player_token,
                       LOCAL_PLAYER.session_token, 8);
                mst->ledger[idx].balance = 0.0f;
                mst->ledger[idx].lifetime_supply = 0.0f;
            }
            if (idx >= 0) mst->ledger[idx].balance += payout;
        }
        set_notice("+$%d  %s %s",
                   (int)lroundf(payout),
                   mining_grade_label(row->grade),
                   commodity_short_name(row->commodity));
    }
}

/* Tow mode: server snaps to the closest slot on E. */
static void sample_placement_tow(input_intent_t *intent) {
    g.placement_reticle_active = false;
    if (!is_key_pressed(SAPP_KEYCODE_E)) return;
    reticle_target_t targets[RETICLE_MAX_TARGETS];
    int n = collect_reticle_targets(LOCAL_PLAYER.ship.pos, targets, RETICLE_MAX_TARGETS);
    intent->place_outpost = true;
    if (n > 0) {
        intent->place_target_station = (int8_t)targets[0].station;
        intent->place_target_ring = (int8_t)targets[0].ring;
        intent->place_target_slot = (int8_t)targets[0].slot;
    }
    /* No outpost in range falls through with all -1 sentinels — server
     * decides whether to materialize a nearby planned station or found
     * a new outpost from scratch. */
    set_notice("Placing scaffold...");
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
        if (s >= 3 && s < MAX_STATIONS &&
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
    set_notice("Station locked! [R] type [E] place [B] exit");
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
        set_notice("Plan cleared. [R] type [E] place [B] exit");
        return;
    }
    intent->add_plan = true;
    intent->plan_station = (int8_t)ps;
    intent->plan_ring = (int8_t)pr;
    intent->plan_slot = (int8_t)psl;
    intent->plan_type = (module_type_t)g.plan_type;
    set_notice("Planned %s. [R] type [E] place [B] exit",
               module_type_name((module_type_t)g.plan_type));
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
        set_notice("Plan limit %d types. Cancel one first.", PLAYER_PLAN_TYPE_LIMIT);
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
        set_notice("Plan: [R] type [E] place [B] exit");
    } else {
        g.plan_mode_active = true;
        g.plan_target_station = -1; /* sentinel: ghost */
        g.placement_target_station = -1;
        g.placement_target_ring = 1;
        g.placement_target_slot = 0;
        set_notice("Plan: [R] type [E] lock [B] exit");
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

/* [ ] keys cycle music tracks. */
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
}

/* H: hail ping. Visual ring fires locally so the press feels instant;
 * server decides which (if any) station to respond with based on
 * ship.comm_range. */
static void sample_hail(input_intent_t *intent) {
    if (!is_key_pressed(SAPP_KEYCODE_H) || LOCAL_PLAYER.docked) return;
    intent->hail = true;
    g.hail_ping_timer  = 0.001f; /* any nonzero = active */
    g.hail_ping_origin = LOCAL_PLAYER.ship.pos;
    g.hail_ping_range  = (LOCAL_PLAYER.ship.comm_range > 0.0f)
                         ? LOCAL_PLAYER.ship.comm_range : 1500.0f;
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
    intent.service_sell_only = COMMODITY_COUNT; /* default: deliver all */
    intent.service_sell_grade = MINING_GRADE_COUNT; /* default: any grade */
    intent.service_sell_one = false;

    sample_movement(&intent);
    sample_tractor(&intent);
    sample_boost(&intent);
    sample_self_destruct(&intent);
    sample_ui_safety();
    sample_targeting(&intent);
    sample_e_interact(&intent);
    sample_station_tab();
    sample_yard_keys(&intent);
    sample_work_keys(&intent);
    sample_dock_keys(&intent);
    sample_trade_sell_all(&intent);
    sample_trade_picker(&intent);
    sample_placement(&intent);
    sample_music();
    sample_hail(&intent);
    sample_autopilot(&intent);
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
        } else if (intent->service_sell && intent->service_sell_only < COMMODITY_COUNT) {
            g.pending_net_action = NET_ACTION_DELIVER_COMMODITY + (uint8_t)intent->service_sell_only;
            /* Per-row sell rides the same 5th-byte slot as buy_grade.
             * Server treats `grade < MINING_GRADE_COUNT` as the
             * single-unit signal; bulk-sell branches keep the sentinel
             * so the existing behavior is unchanged. */
            if (intent->service_sell_one && intent->service_sell_grade < MINING_GRADE_COUNT)
                g.pending_net_buy_grade = (uint8_t)intent->service_sell_grade;
        }
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
        else if (intent->place_outpost) {
            g.pending_net_action = 8;
            g.pending_net_place_station = intent->place_target_station;
            g.pending_net_place_ring    = intent->place_target_ring;
            g.pending_net_place_slot    = intent->place_target_slot;
        }
        else if (intent->buy_scaffold_kit && (uint8_t)intent->scaffold_kit_module < MODULE_COUNT)
            g.pending_net_action = NET_ACTION_BUY_SCAFFOLD_TYPED + (uint8_t)intent->scaffold_kit_module;
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
