/*
 * station_ui.c -- Station lookup helpers, formatting, and the docked station
 * services text renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "palette.h"
#include "mining_client.h"
#include "contract_objective.h"
#include "manifest.h"
#include "cargo_lineage.h"
#include "station_authority.h"
#include "contract_fit.h"
/* Grade palette lives in shared/mining.h (pulled in via client.h →
 * types.h → mining.h) alongside the grade enum + label + multiplier. */

static const uint8_t COL_TRACKED_JOB[3] = { 255, 222, 51 };

/* ------------------------------------------------------------------ */
/* Station lookup helpers                                              */
/* ------------------------------------------------------------------ */

const station_t* station_at(int station_index) {
    if ((station_index < 0) || (station_index >= MAX_STATIONS)) {
        return NULL;
    }
    return &g.world.stations[station_index];
}

const station_t* current_station_ptr(void) {
    return station_at(LOCAL_PLAYER.current_station);
}

const station_t* nearby_station_ptr(void) {
    return station_at(LOCAL_PLAYER.nearby_station);
}

int nearest_station_index(vec2 pos) {
    float best_distance_sq = 0.0f;
    int best_index = -1;

    for (int i = 0; i < MAX_STATIONS; i++) {
        float distance_sq = v2_dist_sq(pos, g.world.stations[i].pos);
        if ((best_index < 0) || (distance_sq < best_distance_sq)) {
            best_distance_sq = distance_sq;
            best_index = i;
        }
    }

    return best_index;
}

int player_planned_types(module_type_t *out, int max) {
    if (!out || max <= 0) return 0;
    /* Faction-shared blueprints: count distinct types across all
     * planned slots regardless of who placed them. */
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        for (int p = 0; p < st->placement_plan_count; p++) {
            module_type_t t = st->placement_plans[p].type;
            bool dup = false;
            for (int k = 0; k < count; k++) {
                if (out[k] == t) { dup = true; break; }
            }
            if (dup) continue;
            if (count >= max) return count;
            out[count++] = t;
        }
    }
    return count;
}

const station_t* navigation_station_ptr(void) {
    if (LOCAL_PLAYER.docked) {
        return current_station_ptr();
    }
    if (LOCAL_PLAYER.nearby_station >= 0) {
        return nearby_station_ptr();
    }
    return station_at(nearest_station_index(LOCAL_PLAYER.ship.pos));
}

/* ------------------------------------------------------------------ */
/* Station role labels and colors                                      */
/* ------------------------------------------------------------------ */

const char* station_role_name(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "Refinery";
        case MODULE_FRAME_PRESS: return "Yard";
        case MODULE_LASER_FAB:   return "Beamworks";
        case MODULE_TRACTOR_FAB: return "Beamworks";
        case MODULE_SIGNAL_RELAY:return "Outpost";
        default:                 return "Station";
    }
}

const char* station_role_short_name(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "REF";
        case MODULE_FRAME_PRESS: return "YARD";
        case MODULE_LASER_FAB:   return "BEAM";
        case MODULE_TRACTOR_FAB: return "BEAM";
        case MODULE_SIGNAL_RELAY:return "OTP";
        default:                 return "STN";
    }
}

const char* station_role_hub_label(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "REFINERY // smelter";
        case MODULE_FRAME_PRESS: return "YARD // frame bay";
        case MODULE_LASER_FAB:   return "BEAMWORKS // field bench";
        case MODULE_TRACTOR_FAB: return "BEAMWORKS // field bench";
        case MODULE_SIGNAL_RELAY:return "OUTPOST // relay hub";
        default:                 return "STATION";
    }
}

void station_role_color(const station_t* station, float* r, float* g0, float* b) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     PAL_UNPACK3(PAL_MODULE_FURNACE,     *r, *g0, *b); break;
        case MODULE_FRAME_PRESS: PAL_UNPACK3(PAL_MODULE_FRAME_PRESS, *r, *g0, *b); break;
        case MODULE_LASER_FAB:   PAL_UNPACK3(PAL_MODULE_LASER_FAB,   *r, *g0, *b); break;
        case MODULE_TRACTOR_FAB: PAL_UNPACK3(PAL_MODULE_TRACTOR_FAB, *r, *g0, *b); break;
        case MODULE_SIGNAL_RELAY:PAL_UNPACK3(PAL_MODULE_SIGNAL_RELAY, *r, *g0, *b); break;
        default:                 PAL_UNPACK3(PAL_STATION_NEUTRAL,     *r, *g0, *b); break;
    }
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

static int station_manifest_count_c(const station_t *st, commodity_t commodity);
static int ship_manifest_count_c(const ship_t *ship, commodity_t commodity);
static float ship_manifest_backed_cargo_volume(const ship_t *ship);
static bool can_afford_upgrade_manifest_ui(const station_t *station,
                                           const ship_t *ship,
                                           ship_upgrade_t upgrade,
                                           float balance);

static const NetDeliveryLedgerEntry *ui_delivery_ledger_for_contract(
    int contract_index)
{
    for (int i = 0; i < g.delivery_ledger_count; i++) {
        const NetDeliveryLedgerEntry *entry = &g.delivery_ledger[i];
        if (entry->contract_index == (uint8_t)contract_index)
            return entry;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* WORK-tab contract slot builder                                      */
/* ------------------------------------------------------------------ */

int build_work_slots(int here_idx, vec2 here_pos,
                     int out_contracts[3],
                     bool out_fulfillable[3],
                     int out_held[3])
{
    for (int i = 0; i < 3; i++) {
        out_contracts[i]   = -1;
        out_fulfillable[i] = false;
        out_held[i]        = 0;
    }
    int count = 0;

    /* Pass 1: contracts at this station the player can act on
     * right now. Raw ore lives in towed S-tier fragments; finished goods
     * are counted from the ship manifest. */
    for (int ci = 0; ci < MAX_CONTRACTS && count < 3; ci++) {
        /* Gossip filter: only show contracts the player has heard
         * about via dock contact. Mask is set by the per-player
         * NET_MSG_PLAYER_KNOWN_CONTRACTS message. */
        if (!(g.player_known_contract_mask & (1u << ci))) continue;
        const contract_t *ct = &g.world.contracts[ci];
        if (!ct->active) continue;
        int held_int = 0;
        bool actionable_here = false;
        if (ct->action == CONTRACT_DELIVERY) {
            const NetDeliveryLedgerEntry *ledger =
                ui_delivery_ledger_for_contract(ci);
            bool at_origin = here_idx >= 0 && ct->target_index == here_idx;
            bool at_dest = here_idx >= 0 && ct->station_index == here_idx;
            held_int = contract_fit_manifest_count(ct,
                                                   &LOCAL_PLAYER.ship.manifest);
            if (at_origin && (!ledger ||
                              ledger->status == DELIVERY_SHIPMENT_DELIVERED)) {
                actionable_here = true;
                if (held_int <= 0) {
                    held_int = ledger && ledger->quantity_total > 0
                        ? (int)ledger->quantity_total
                        : (int)ceilf(ct->quantity_needed);
                }
            } else if (at_dest && ledger &&
                       ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
                       held_int > 0) {
                actionable_here = true;
            }
        } else if (ct->action == CONTRACT_TRACTOR) {
            if (here_idx < 0 || ct->station_index != here_idx) continue;
            if (ct->commodity < COMMODITY_RAW_ORE_COUNT) {
                float held_ore = 0.0f;
                const ship_t *ship = &LOCAL_PLAYER.ship;
                for (int t = 0; t < ship->towed_count; t++) {
                    int fi = ship->towed_fragments[t];
                    if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
                    const asteroid_t *a = &g.world.asteroids[fi];
                    if (!contract_fit_is_ok(contract_fit_fragment(ct, a))) continue;
                    held_ore += a->ore;
                }
                held_int = (int)lroundf(held_ore);
            } else {
                held_int = contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest);
            }
            actionable_here = held_int > 0;
        }
        if (!actionable_here) continue;
        out_contracts[count]   = ci;
        out_fulfillable[count] = true;
        out_held[count]        = held_int;
        count++;
    }

    /* Pass 2: fill any remaining slots with the nearest active
     * contracts (any station) by squared distance from `here_pos`,
     * skipping anything already in pass 1. */
    if (count < 3) {
        int nearest[3] = {-1, -1, -1};
        float nearest_d[3] = {1e18f, 1e18f, 1e18f};
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            if (!(g.player_known_contract_mask & (1u << ci))) continue; /* gossip filter */
            const contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->action == CONTRACT_DELIVERY) {
                if (ct->station_index >= MAX_STATIONS ||
                    ct->target_index < 0 ||
                    ct->target_index >= MAX_STATIONS) {
                    continue;
                }
                if (!station_exists(&g.world.stations[ct->station_index]) ||
                    !station_exists(&g.world.stations[ct->target_index])) {
                    continue;
                }
            } else {
                if (ct->station_index >= MAX_STATIONS) continue;
                if (!station_exists(&g.world.stations[ct->station_index])) continue;
            }
            bool already = false;
            for (int s = 0; s < count; s++)
                if (out_contracts[s] == ci) { already = true; break; }
            if (already) continue;
            vec2 target = ct->target_pos;
            if (ct->action == CONTRACT_TRACTOR) {
                target = g.world.stations[ct->station_index].pos;
            } else if (ct->action == CONTRACT_DELIVERY) {
                const NetDeliveryLedgerEntry *ledger =
                    ui_delivery_ledger_for_contract(ci);
                int station_idx = ct->target_index;
                if (ledger && ledger->status == DELIVERY_SHIPMENT_PICKED_UP)
                    station_idx = ct->station_index;
                if (ledger && ledger->status == DELIVERY_SHIPMENT_DELIVERED)
                    station_idx = ct->target_index;
                target = g.world.stations[station_idx].pos;
            }
            float d = v2_dist_sq(here_pos, target);
            for (int s = 0; s < 3; s++) {
                if (d < nearest_d[s]) {
                    for (int j = 2; j > s; j--) {
                        nearest[j] = nearest[j-1];
                        nearest_d[j] = nearest_d[j-1];
                    }
                    nearest[s] = ci;
                    nearest_d[s] = d;
                    break;
                }
            }
        }
        for (int s = 0; s < 3 && count < 3; s++) {
            if (nearest[s] < 0) continue;
            out_contracts[count]   = nearest[s];
            out_fulfillable[count] = false;
            out_held[count]        = 0;
            count++;
        }
    }

    return count;
}

/* ------------------------------------------------------------------ */
/* Station UI state builder                                            */
/* ------------------------------------------------------------------ */

void build_station_ui_state(station_ui_state_t* ui) {
    memset(ui, 0, sizeof(*ui));
    ui->station = current_station_ptr();
    if (!ui->station) {
        return;
    }

    ui->hull_now = (int)lroundf(LOCAL_PLAYER.ship.hull);
    ui->hull_max = (int)lroundf(ship_max_hull(&LOCAL_PLAYER.ship));
    float repair = station_repair_cost(&LOCAL_PLAYER.ship, current_station_ptr());
    ui->repair_cost = (int)lroundf(repair);

    /* Compute per-upgrade module accounting (cargo first, dock fallback).
     * Finished goods are manifest-backed; floats are only derived caches. */
    struct { ship_upgrade_t up; int *needed, *cargo, *atstation, *credit; } slots[3] = {
        { SHIP_UPGRADE_MINING,
          &ui->mining_units_needed,  &ui->mining_units_in_cargo,
          &ui->mining_units_at_station, &ui->mining_credit_cost },
        { SHIP_UPGRADE_HOLD,
          &ui->hold_units_needed,    &ui->hold_units_in_cargo,
          &ui->hold_units_at_station,   &ui->hold_credit_cost },
        { SHIP_UPGRADE_TRACTOR,
          &ui->tractor_units_needed, &ui->tractor_units_in_cargo,
          &ui->tractor_units_at_station,&ui->tractor_credit_cost },
    };
    for (int i = 0; i < 3; i++) {
        commodity_t c = (commodity_t)(COMMODITY_FRAME +
                        upgrade_required_product(slots[i].up));
        int need = (int)ceilf(upgrade_product_cost(&LOCAL_PLAYER.ship, slots[i].up));
        int in_cargo  = ship_manifest_count_c(&LOCAL_PLAYER.ship, c);
        int at_station = ui->station ? station_manifest_count_c(ui->station, c) : 0;
        int from_station = need - (need < in_cargo ? need : in_cargo);
        if (from_station < 0) from_station = 0;
        float credit = ui->station
            ? (float)from_station * station_sell_price(ui->station, c) : 0.0f;
        *slots[i].needed    = need;
        *slots[i].cargo     = in_cargo;
        *slots[i].atstation = at_station;
        *slots[i].credit    = (int)lroundf(credit);
    }
    /* Any dock installs kits — gate is whether there are kits available
     * (computed below) and whether the quoted cost is affordable. */
    ui->can_repair = (repair > 0.0f) && (player_current_balance() + FLOAT_EPSILON >= repair);

    /* Kit availability for the [R] row — drives "X kits ship / Y kits
     * station" hint and the partial-repair warning. */
    ui->ship_kits    = ship_manifest_count_c(&LOCAL_PLAYER.ship,
                                             COMMODITY_REPAIR_KIT);
    ui->station_kits = (ui->station)
        ? station_manifest_count_c(ui->station, COMMODITY_REPAIR_KIT) : 0;
    int hp_needed = ui->hull_max - ui->hull_now;
    if (hp_needed < 0) hp_needed = 0;
    int kits_avail = ui->ship_kits + ui->station_kits;
    ui->kits_short_by = (hp_needed > kits_avail) ? (hp_needed - kits_avail) : 0;
    float bal = player_current_balance();
    ui->can_upgrade_mining =
        can_afford_upgrade_manifest_ui(ui->station, &LOCAL_PLAYER.ship,
                                       SHIP_UPGRADE_MINING, bal);
    ui->can_upgrade_hold =
        can_afford_upgrade_manifest_ui(ui->station, &LOCAL_PLAYER.ship,
                                       SHIP_UPGRADE_HOLD, bal);
    ui->can_upgrade_tractor =
        can_afford_upgrade_manifest_ui(ui->station, &LOCAL_PLAYER.ship,
                                       SHIP_UPGRADE_TRACTOR, bal);
}

/* ------------------------------------------------------------------ */
/* Currency + ingot stock helpers                                      */
/* ------------------------------------------------------------------ */

/* Station-local currency label, falls back to "cr". */
static const char *ui_station_currency(const station_t *st) {
    if (!st) return "cr";
    return (st->currency_name[0]) ? st->currency_name : "cr";
}

/* Compact form of the currency name — first word, lowercased and
 * trimmed to <= 4 chars. Used by the header band when the full label
 * ("prospect vouchers", 17 chars) won't fit. Falls back to "cr".
 * Caller-owned buffer must be >= 5 bytes. */
static void ui_station_currency_short(const station_t *st, char *out, size_t cap) {
    if (cap == 0) return;
    if (!st || !st->currency_name[0]) {
        snprintf(out, cap, "cr");
        return;
    }
    /* Take the first whitespace-delimited word. "prospect vouchers" -> "prospect" */
    size_t i = 0, j = 0;
    while (st->currency_name[i] != '\0' && st->currency_name[i] != ' ' &&
           j < cap - 1 && j < 4) {
        char c = st->currency_name[i];
        out[j++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        i++;
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, cap, "cr");
}

static void ui_station_name_short(int station_index, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (station_index < 0 || station_index >= MAX_STATIONS) {
        snprintf(out, cap, "station");
        return;
    }
    const station_t *st = &g.world.stations[station_index];
    if (!station_exists(st) || !st->name[0]) {
        snprintf(out, cap, "station");
        return;
    }
    size_t i = 0, j = 0;
    while (st->name[i] != '\0' && st->name[i] != ' ' && j < cap - 1) {
        out[j++] = st->name[i++];
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, cap, "station");
}

static void ui_fit_text(const char *src, int max_chars, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!src || max_chars <= 0) return;

    size_t limit = (size_t)max_chars;
    if (limit >= cap) limit = cap - 1;
    size_t len = strlen(src);
    if (len <= limit) {
        snprintf(out, cap, "%s", src);
        return;
    }

    if (limit <= 3) {
        for (size_t i = 0; i < limit; i++) out[i] = '.';
        out[limit] = '\0';
        return;
    }

    snprintf(out, cap, "%.*s...", (int)(limit - 3), src);
}

/* ====================================================================
 * STATION DOCKED UI — redesigned (#redesign)
 *
 * Layout:
 *   ┌─ persistent header band (always rendered) ─┐
 *   │ name · role · ledger here · signal · LAUNCH │
 *   │ hull X/Y · cargo X/Y                        │
 *   │ ⌁ "ticker line" (faded)                     │
 *   ├─────────────────────────────────────────────┤
 *   │ view content (one of three):                │
 *   │   VERBS — verb-list action surface (default)│
 *   │   JOBS  — contract picker (entered via [J]) │
 *   │   BUILD — module-order picker (via [B])     │
 *   └─────────────────────────────────────────────┘
 *
 * Every visible row in the verb-list maps to a single keypress that
 * actually does something at THIS dock. Locked / unaffordable / out-
 * of-stock options simply don't render — no greyed-out aspirational
 * data, no information-without-action.
 * ==================================================================== */

static void draw_header_band(const station_ui_state_t *ui,
                             float panel_x, float panel_y,
                             float panel_w, bool compact)
{
    const station_t *st = ui->station;
    float left_x = panel_x + 20.0f;
    float right_margin = 20.0f;
    const float cell_w = 8.0f;

    /* Header lines sit below the outer panel's top chrome (corner brackets
     * and title rule at y+14). Keep line 1 >= panel_y + 22 to avoid clipping.
     * Layout (per redesign):
     *   Line 1: station name (left)   ·   [E] LAUNCH (right)
     *   Line 2: station role (left)   ·   ledger N cur · sig X.XX (right)
     *   Line 3: ticker (full width)
     * Ship hull/cargo/modules live in the footer + the DOCK tab. */
    const float HEADER_L1 = 26.0f;
    const float HEADER_L2 = 42.0f;
    const float HEADER_L3 = 58.0f;

    /* Line 1: station name (+ Ed25519 pubkey prefix) (left)  ·
     *         [E] LAUNCH (right).
     * The pubkey suffix lets the player visually confirm they're
     * docked at a legitimately-keyed station, not a spoof with the
     * same name (#479 B). */
    sdtx_color3b(PAL_TEXT_PRIMARY);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L1));
    {
        const char *launch = "[E] LAUNCH";
        float launch_w = (panel_w >= 360.0f) ? (float)strlen(launch) * cell_w : 0.0f;
        float title_right = panel_x + panel_w - right_margin
                          - (launch_w > 0.0f ? launch_w + 16.0f : 0.0f);
        int title_chars = (int)floorf((title_right - left_x) / cell_w);
        char name_with_pub[96];
        char name_only[64];
        char title_fit[96];
        char pub_prefix[16];
        station_pubkey_b58_prefix(st, pub_prefix);
        if (pub_prefix[0])
            snprintf(name_with_pub, sizeof(name_with_pub), "%s (%s...)",
                     st->name, pub_prefix);
        else
            snprintf(name_with_pub, sizeof(name_with_pub), "%s", st->name);
        snprintf(name_only, sizeof(name_only), "%s", st->name);

        const char *title = (!compact && (int)strlen(name_with_pub) <= title_chars)
            ? name_with_pub : name_only;
        ui_fit_text(title, title_chars, title_fit, sizeof(title_fit));
        sdtx_puts(title_fit);
    }

    if (panel_w >= 360.0f) {
        const char *launch = "[E] LAUNCH";
        float lw = (float)strlen(launch) * cell_w;
        sdtx_pos(ui_text_pos(panel_x + panel_w - right_margin - lw),
                 ui_text_pos(panel_y + HEADER_L1));
        sdtx_color3b(PAL_STATION_HINT);
        sdtx_puts(launch);
    }

    /* Line 2: role (left)  ·  ledger + signal (right).
     * Role labels are long ("BEAMWORKS // field bench"), and the right
     * side can run to ~35 chars. Measure both and skip the right-side
     * data if they'd overlap — compact panels land there. The ledger
     * remains visible in the DOCK tab's SHIP BAY section. */
    const char *role_label = station_role_hub_label(st);
    float right_limit = panel_x + panel_w - right_margin;
    int role_chars = (int)floorf((right_limit - left_x) / cell_w);
    char role_fit[64];
    ui_fit_text(role_label, role_chars, role_fit, sizeof(role_fit));
    float role_w = (float)strlen(role_fit) * cell_w;
    sdtx_color3b(PAL_HOLD_CYAN);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L2));
    sdtx_puts(role_fit);

    if (panel_w >= 360.0f) {
        int balance = (int)lroundf(player_current_balance());
        float sig = signal_strength_at(&g.world, st->pos);
        float gap = 16.0f;
        float left_used = left_x + role_w + gap;

        /* Try four progressively shorter forms, each guaranteed to
         * convey at least the balance. Pick the longest that fits. */
        char short_cur[8];
        ui_station_currency_short(st, short_cur, sizeof(short_cur));
        const char *full_cur = ui_station_currency(st);
        const char *forms[4];
        char buf0[64], buf1[64], buf2[64], buf3[32];
        snprintf(buf0, sizeof(buf0), "ledger %d %s   sig %.2f",  balance, full_cur,  sig);
        snprintf(buf1, sizeof(buf1), "ledger %d %s",             balance, full_cur);
        snprintf(buf2, sizeof(buf2), "%d %s   sig %.2f",         balance, short_cur, sig);
        snprintf(buf3, sizeof(buf3), "%d %s",                    balance, short_cur);
        forms[0] = buf0; forms[1] = buf1; forms[2] = buf2; forms[3] = buf3;

        for (int i = 0; i < 4; i++) {
            float w = (float)strlen(forms[i]) * cell_w;
            if (left_used + w > right_limit) continue;
            sdtx_pos(ui_text_pos(right_limit - w),
                     ui_text_pos(panel_y + HEADER_L2));
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_puts(forms[i]);
            break;
        }
    }

    /* Line 3: ticker — most recent station chatter (replaces NETWORK tab). */
    {
        const signal_channel_t *ch = &g.world.signal_channel;
        if (ch->count > 0) {
            int slot_idx = ch->count - 1;
            int start = (ch->head - ch->count + SIGNAL_CHANNEL_CAPACITY) % SIGNAL_CHANNEL_CAPACITY;
            int slot = (start + slot_idx) % SIGNAL_CHANNEL_CAPACITY;
            const signal_channel_msg_t *m = &ch->msgs[slot];
            const char *sender = "SYSTEM";
            if (m->sender_station >= 0 && m->sender_station < MAX_STATIONS
                && station_exists(&g.world.stations[m->sender_station])) {
                sender = g.world.stations[m->sender_station].name;
            }
            char line[200];
            snprintf(line, sizeof(line), "[%s] %s", sender, m->text);
            int line_chars = (int)floorf((panel_x + panel_w - right_margin - left_x) / cell_w);
            char line_fit[200];
            ui_fit_text(line, line_chars, line_fit, sizeof(line_fit));
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L3));
            sdtx_puts(line_fit);
        }
    }
}

/* ------------------------------------------------------------------ */
/* VERBS view — the new default action surface                         */
/* ------------------------------------------------------------------ */

/* Small section header: label on the left, faded rule filling the row
 * to the inner right edge. Returns the y-advance consumed. */
static float draw_section_header(float cx, float my, float inner_right,
                                 const char *label, const uint8_t label_rgb[3])
{
    const float cell_w = 8.0f;
    sdtx_color3b(label_rgb[0], label_rgb[1], label_rgb[2]);
    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
    sdtx_puts(label);
    float label_w = (float)strlen(label) * cell_w;
    /* Faded dash rule after the label. */
    sdtx_color3b(PAL_TEXT_FADED);
    int chars_avail = (int)((inner_right - (cx + label_w + 8.0f)) / cell_w);
    if (chars_avail > 0) {
        char rule[128];
        int n = (chars_avail < (int)sizeof(rule) - 1)
              ? chars_avail : (int)sizeof(rule) - 1;
        for (int i = 0; i < n; i++) rule[i] = '-';
        rule[n] = '\0';
        sdtx_pos(ui_text_pos(cx + label_w + 8.0f), ui_text_pos(my));
        sdtx_puts(rule);
    }
    return 14.0f;
}

/* Forward decl — yard view lives below (own tab). */
static void draw_yard_view(const station_ui_state_t *ui,
                           float cx, float cy, float inner_w, bool compact);
void station_panel_input_dock(input_intent_t *intent);
void station_panel_input_trade(input_intent_t *intent);
void station_panel_input_work(input_intent_t *intent);
void station_panel_input_yard(input_intent_t *intent);

/* ------------------------------------------------------------------ */
/* Row grammar — every tab uses this shape:                            */
/*   left (hotkey + verb/label)    middle optional    right status     */
/* Monospace cell width is 8 px.                                       */
/* ------------------------------------------------------------------ */

/* Two-column row: left-aligned label at cx, right-aligned status at
 * inner_right. Either side may be NULL to skip it. */
static void draw_row_lr(float cx, float my, float inner_right,
                        const uint8_t left_rgb[3], const char *left_txt,
                        const uint8_t right_rgb[3], const char *right_txt)
{
    const float cell_w = 8.0f;
    char right_fit[96];
    const char *right_draw = NULL;
    float right_w = 0.0f;
    float right_x = inner_right;

    if (right_txt && right_txt[0] && right_rgb) {
        int right_chars = (int)floorf((inner_right - cx) / cell_w);
        ui_fit_text(right_txt, right_chars, right_fit, sizeof(right_fit));
        if (right_fit[0]) {
            right_draw = right_fit;
            right_w = (float)strlen(right_draw) * cell_w;
            right_x = inner_right - right_w;
        }
    }

    if (left_txt && left_txt[0]) {
        char left_fit[96];
        const char *left_draw = left_fit;
        int left_chars;
        if (right_draw) {
            left_chars = (int)floorf((right_x - cx - 8.0f) / cell_w);
        } else {
            left_chars = (int)floorf((inner_right - cx) / cell_w);
        }
        ui_fit_text(left_txt, left_chars, left_fit, sizeof(left_fit));
        if (left_draw[0]) {
            sdtx_color3b(left_rgb[0], left_rgb[1], left_rgb[2]);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            sdtx_puts(left_draw);
        }
    }
    if (right_draw && right_rgb) {
        sdtx_color3b(right_rgb[0], right_rgb[1], right_rgb[2]);
        sdtx_pos(ui_text_pos(right_x), ui_text_pos(my));
        sdtx_puts(right_draw);
    }
}

/* Cell-grid row: writes each field at a fixed column offset (in chars)
 * measured from cx. NULL field = skip. */
typedef struct { int col; const char *text; const uint8_t *rgb; } cell_t;
static void draw_row_cells(float cx, float my, const cell_t *cells, int n)
{
    const float cell_w = 8.0f;
    for (int i = 0; i < n; i++) {
        if (!cells[i].text || !cells[i].text[0]) continue;
        sdtx_color3b(cells[i].rgb[0], cells[i].rgb[1], cells[i].rgb[2]);
        sdtx_pos(ui_text_pos(cx + (float)cells[i].col * cell_w), ui_text_pos(my));
        sdtx_puts(cells[i].text);
    }
}

/* Section-header color families, shared by DOCK and TRADE views. */
static const uint8_t HDR_TRADE[3]   = { PAL_CONTRACT_AFFORD };
static const uint8_t HDR_SERVICE[3] = { PAL_ORE_AMBER };
static const uint8_t HDR_FIT[3]     = { PAL_NAV_BLUE };
static const uint8_t HDR_YARD[3]    = { PAL_HOLD_CYAN };

/* Station manifest readers — unified through the client-side summary
 * (g.station_manifest_summary) populated every frame in SP and by the
 * net sync in MP. UI no longer pokes at station_t.manifest directly;
 * the summary is the only contract. */
static int station_index_of(const station_t *st) {
    return (int)(st - g.world.stations);
}

void reset_trade_session_rows(int station_index) {
    g.trade_session_station = station_index;
    g.trade_page = 0;
    memset(g.trade_seen_buy, 0, sizeof(g.trade_seen_buy));
    memset(g.trade_seen_sell, 0, sizeof(g.trade_seen_sell));
}

static int trade_session_station_index(const station_t *st) {
    if (!st) return -1;
    int s = station_index_of(st);
    if (s < 0 || s >= MAX_STATIONS) return -1;
    if (g.trade_session_station != s) reset_trade_session_rows(s);
    return s;
}

static int station_manifest_count_cg(const station_t *st,
                                     commodity_t commodity,
                                     mining_grade_t grade)
{
    if (!st) return 0;
    int s = station_index_of(st);
    if (s < 0 || s >= MAX_STATIONS) return 0;
    if ((int)commodity < 0 || (int)commodity >= COMMODITY_COUNT) return 0;
    if ((int)grade < 0 || (int)grade >= MINING_GRADE_COUNT) return 0;
    return (int)g.station_manifest_summary[s][commodity][grade];
}

static int station_manifest_count_c(const station_t *st, commodity_t commodity)
{
    int total = 0;
    for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
        total += station_manifest_count_cg(st, commodity, (mining_grade_t)gi);
    return total;
}

static int manifest_lineage_count_cg(const manifest_t *manifest,
                                     commodity_t commodity,
                                     mining_grade_t grade)
{
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (u->commodity != (uint8_t)commodity || u->grade != (uint8_t)grade) continue;
        if (u->mined_block != 0) n++;
    }
    return n;
}

static bool trade_unit_is_named_ingot(const cargo_unit_t *u)
{
    return u && (cargo_kind_t)u->kind == CARGO_KIND_INGOT &&
           (ingot_prefix_t)u->prefix_class != INGOT_PREFIX_ANONYMOUS;
}

static bool trade_unit_matches_cg(const cargo_unit_t *u,
                                  commodity_t commodity,
                                  mining_grade_t grade)
{
    return u && u->commodity == (uint8_t)commodity &&
           u->grade == (uint8_t)grade;
}

static bool trade_unit_is_market_buy_unit(const cargo_unit_t *u,
                                          commodity_t commodity,
                                          mining_grade_t grade)
{
    if (!trade_unit_matches_cg(u, commodity, grade)) return false;
    return !trade_unit_is_named_ingot(u);
}

static int manifest_named_count_cg_direct(const manifest_t *manifest,
                                          commodity_t commodity,
                                          mining_grade_t grade)
{
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (trade_unit_matches_cg(u, commodity, grade) &&
            trade_unit_is_named_ingot(u)) n++;
    }
    return n;
}

static int manifest_market_count_cg_direct(const manifest_t *manifest,
                                           commodity_t commodity,
                                           mining_grade_t grade)
{
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        if (trade_unit_is_market_buy_unit(&manifest->units[i], commodity, grade)) n++;
    }
    return n;
}

static int manifest_market_lineage_count_cg(const manifest_t *manifest,
                                            commodity_t commodity,
                                            mining_grade_t grade)
{
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (!trade_unit_is_market_buy_unit(u, commodity, grade)) continue;
        if (u->mined_block != 0) n++;
    }
    return n;
}

static bool station_ui_hash_is_zero(const uint8_t hash[32])
{
    return cargo_lineage_hash_is_zero(hash);
}

static bool cargo_unit_has_player_origin(const cargo_unit_t *u)
{
    if (!u) return false;
    if (u->recipe_id == (uint16_t)RECIPE_LEGACY_MIGRATE) return false;
    if ((cargo_kind_t)u->kind != CARGO_KIND_INGOT && u->mined_block == 0)
        return false;
    if (station_short_name((int)u->origin_station)[0] == '?') return false;
    return !station_ui_hash_is_zero(u->pub);
}

static void trade_row_attach_inspect(trade_row_t *row,
                                     const cargo_unit_t *unit,
                                     const cargo_receipt_chain_t *chain)
{
    if (!row || !unit || station_ui_hash_is_zero(unit->pub)) return;
    row->has_inspect = true;
    row->inspect_kind = unit->kind;
    row->inspect_recipe_id = unit->recipe_id;
    row->inspect_chain_len = (chain && chain->len <= CARGO_RECEIPT_CHAIN_MAX_LEN)
        ? chain->len : 0;
    memcpy(row->inspect_pub, unit->pub, sizeof(row->inspect_pub));
    memcpy(row->inspect_parent, unit->parent_merkle, sizeof(row->inspect_parent));
}

static int manifest_find_first_market_cg(const manifest_t *manifest,
                                         commodity_t commodity,
                                         mining_grade_t grade)
{
    if (!manifest || !manifest->units) return -1;
    for (uint16_t i = 0; i < manifest->count; i++) {
        if (trade_unit_is_market_buy_unit(&manifest->units[i], commodity, grade))
            return (int)i;
    }
    return -1;
}

static int station_market_stock_cg(const station_t *st,
                                   commodity_t commodity,
                                   mining_grade_t grade)
{
    int total = station_manifest_count_cg(st, commodity, grade);
    if (commodity == COMMODITY_FERRITE_INGOT ||
        commodity == COMMODITY_CUPRITE_INGOT ||
        commodity == COMMODITY_CRYSTAL_INGOT) {
        int named = manifest_named_count_cg_direct(st ? &st->manifest : NULL,
                                                   commodity, grade);
        total -= named;
    }
    return total > 0 ? total : 0;
}

/* station_manifest_has_commodity / ship_manifest_has_commodity removed —
 * after the manifest-first TRADE rewrite the rows always probe the
 * full grade range directly, so the "any-grade?" predicate is no
 * longer needed. */

/* Ship manifest helpers — iterate directly. In multiplayer, net_sync.c
 * rebuilds the local ship manifest from PLAYER_MANIFEST plus any detailed
 * HOLD_INGOTS provenance snapshot, so the trade UI can read the same
 * local manifest path in SP and MP. */
static int ship_manifest_count_cg(const ship_t *ship,
                                  commodity_t commodity,
                                  mining_grade_t grade)
{
    if (!ship || !ship->manifest.units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (u->commodity == (uint8_t)commodity && u->grade == (uint8_t)grade) n++;
    }
    return n;
}

static int ship_manifest_count_c(const ship_t *ship, commodity_t commodity)
{
    return manifest_count_by_commodity(ship ? &ship->manifest : NULL, commodity);
}

static float ship_manifest_backed_cargo_volume(const ship_t *ship)
{
    if (!ship) return 0.0f;
    float total = 0.0f;
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++)
        total += ship->cargo[c] * commodity_volume((commodity_t)c);
    if (ship->manifest.units) {
        for (uint16_t u = 0; u < ship->manifest.count; u++) {
            const cargo_unit_t *cu = &ship->manifest.units[u];
            if (cu->commodity >= COMMODITY_COUNT) continue;
            total += commodity_volume((commodity_t)cu->commodity);
        }
    }
    return total;
}

static bool can_afford_upgrade_manifest_ui(const station_t *station,
                                           const ship_t *ship,
                                           ship_upgrade_t upgrade,
                                           float balance)
{
    if (!station || !ship) return false;
    if (ship_upgrade_maxed(ship, upgrade)) return false;
    commodity_t comm = (commodity_t)(COMMODITY_FRAME + upgrade_required_product(upgrade));
    int units_needed = (int)ceilf(upgrade_product_cost(ship, upgrade));
    int in_cargo = ship_manifest_count_c(ship, comm);
    int at_station = station_manifest_count_c(station, comm);
    if (in_cargo + at_station < units_needed) return false;
    int from_station = units_needed - (units_needed < in_cargo ? units_needed : in_cargo);
    float credit_cost = (float)from_station * station_sell_price(station, comm);
    return balance + FLOAT_EPSILON >= credit_cost;
}

/* Representative SELL unit for a (commodity, grade) row. The server's
 * per-row sell path picks the highest prefix multiplier in the bucket,
 * so the UI quotes that same unit instead of an arbitrary FIFO unit. */
static int manifest_find_top_sell_unit_cg(const manifest_t *manifest,
                                          commodity_t commodity,
                                          mining_grade_t grade)
{
    if (!manifest || !manifest->units) return -1;
    int top_idx = -1;
    float top_mult = 1.0f;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (!trade_unit_matches_cg(u, commodity, grade)) continue;
        float m = prefix_class_price_multiplier((int)u->prefix_class);
        if (top_idx < 0 || m > top_mult) {
            top_mult = m;
            top_idx = (int)i;
        }
    }
    return top_idx;
}

/* TRADE view — market. Unified list of rows; BUY rows (what the station
 * sells) first, then SELL rows (what the station buys). Hotkeys [1]..[5]
 * select a row on the current page. [F] pages forward; pages wrap at
 * the last page so the UI never runs out. At-station trades show the
 * generic "$" symbol — contract payouts in WORK still name the issuing
 * station's currency because that's where the money actually lives.
 *
 * trade_row_t and the pagination constants live in client.h so input.c
 * can index into the same row list — see build_trade_rows below. */

int build_trade_rows(const station_t *st, const ship_t *ship,
                     trade_row_t out[], int max) {
    if (!st || !ship || !out || max <= 0) return 0;
    if (trade_session_station_index(st) < 0) return 0;
    int row_count = 0;
    float free_volume = ship_cargo_capacity(ship) - ship_manifest_backed_cargo_volume(ship);
    float credits = player_current_balance();
    int capacity = (int)lroundf(MAX_PRODUCT_STOCK);

    /* BUY rows -- one per (commodity, grade) the station produces.
     * Rows are additive per dock session: an in-stock grade appears
     * as soon as it exists, then remains visible if the player buys
     * the shelf empty. Never-before-seen empty catalog lines stay
     * hidden. */
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT && row_count < max; c++) {
        if (!station_produces(st, (commodity_t)c)) continue;
        float price_base = station_sell_price(st, (commodity_t)c);
        if (price_base <= FLOAT_EPSILON) continue;
        for (int gi = 0; gi < MINING_GRADE_COUNT && row_count < max; gi++) {
            int stock = station_market_stock_cg(st, (commodity_t)c, (mining_grade_t)gi);
            if (stock > 0)
                g.trade_seen_buy[c][gi] = true;
            if (stock <= 0 && !g.trade_seen_buy[c][gi]) continue;
            int price = (int)lroundf(price_base
                    * mining_payout_multiplier((mining_grade_t)gi));
            float vol = commodity_volume((commodity_t)c);
            int per_press = (vol > FLOAT_EPSILON) ? (int)lroundf(1.0f / vol) : 1;
            if (per_press < 1) per_press = 1;
            int volume_cap = (vol > FLOAT_EPSILON)
                ? (int)floorf((free_volume + FLOAT_EPSILON) / vol) : per_press;
            int funds_cap = (price > 0)
                ? (int)floorf((credits + FLOAT_EPSILON) / (float)price) : 0;
            int qty = per_press;
            if (qty > stock) qty = stock;
            if (qty > volume_cap) qty = volume_cap;
            if (qty > funds_cap) qty = funds_cap;
            uint8_t blk = TRADE_BLOCK_NONE;
            if (stock <= 0) blk = TRADE_BLOCK_STATION_EMPTY;
            else if (volume_cap <= 0) blk = TRADE_BLOCK_HOLD_FULL;
            else if (funds_cap <= 0) blk = TRADE_BLOCK_NO_FUNDS;
            /* Per-grade stock is what the row offers; that's already
             * `stock` from the manifest count above. The total cap is
             * shared with other grades of this commodity, so the
             * passive line below also references the per-commodity
             * inventory. */
            /* Pull representative-unit lineage from the station's
             * manifest only when the whole row has provenance locally.
             * In MP, station manifests are partial named-ingot mirrors;
             * if anonymous cargo shares the bucket, we hide lineage
             * rather than imply the next BUY is known. */
            uint8_t origin_idx = 0;
            uint64_t mined_blk = 0;
            bool has_lineage = false;
            const cargo_unit_t *inspect_unit = NULL;
            const cargo_receipt_chain_t *inspect_chain = NULL;
            int represented = manifest_market_count_cg_direct(&st->manifest,
                                                              (commodity_t)c,
                                                              (mining_grade_t)gi);
            int lineage_count = manifest_market_lineage_count_cg(&st->manifest,
                                                                 (commodity_t)c,
                                                                 (mining_grade_t)gi);
            if (represented == stock) {
                int rep_idx = manifest_find_first_market_cg(&st->manifest,
                                                            (commodity_t)c,
                                                            (mining_grade_t)gi);
                if (rep_idx >= 0) {
                    const cargo_unit_t *rep = &st->manifest.units[rep_idx];
                    origin_idx = rep->origin_station;
                    mined_blk = rep->mined_block;
                    has_lineage = (lineage_count == stock && mined_blk != 0) ||
                                  cargo_unit_has_player_origin(rep);
                    inspect_unit = rep;
                    const ship_receipts_t *rcpts = station_get_receipts_const(st);
                    if (rcpts && (uint16_t)rep_idx < rcpts->count)
                        inspect_chain = &rcpts->chains[rep_idx];
                }
            }
            trade_row_t row = (trade_row_t){
                .kind = 0, .commodity = (commodity_t)c, .grade = (mining_grade_t)gi,
                .stock = stock, .quantity = qty,
                .unit_price = price, .total_price = price * qty,
                .actionable = (blk == TRADE_BLOCK_NONE && qty > 0),
                .station_stock = stock, .station_capacity = capacity,
                .held = 0, .block_reason = blk,
                .has_lineage = has_lineage,
                .origin_station_idx = origin_idx,
                .mined_block = mined_blk,
            };
            trade_row_attach_inspect(&row, inspect_unit, inspect_chain);
            out[row_count++] = row;
        }
    }

    /* SELL rows -- every commodity/grade the player has carried during
     * this dock session for a commodity the station consumes. Rows appear
     * when cargo exists and remain visible after the player sells it all,
     * but never-before-carried cargo stays hidden. */
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT && row_count < max; c++) {
        if (!station_consumes(st, (commodity_t)c)) continue;
        float price_base = station_buy_price(st, (commodity_t)c);
        if (price_base <= FLOAT_EPSILON) continue;
        int station_total_inv = station_manifest_count_c(st, (commodity_t)c);
        bool station_full = station_total_inv >= capacity;
        for (int gi = 0; gi < MINING_GRADE_COUNT && row_count < max; gi++) {
            int manifest_g = ship_manifest_count_cg(ship, (commodity_t)c, (mining_grade_t)gi);
            if (manifest_g > 0)
                g.trade_seen_sell[c][gi] = true;
            if (manifest_g <= 0 && !g.trade_seen_sell[c][gi]) continue;
            int held = manifest_g;
            int rep_idx = manifest_find_top_sell_unit_cg(&ship->manifest,
                                                         (commodity_t)c,
                                                         (mining_grade_t)gi);
            int top_cls = (int)INGOT_PREFIX_ANONYMOUS;
            if (rep_idx >= 0)
                top_cls = (int)ship->manifest.units[rep_idx].prefix_class;
            float prefix_mult = prefix_class_price_multiplier(top_cls);
            int price = (int)lroundf(price_base
                    * mining_payout_multiplier((mining_grade_t)gi)
                    * prefix_mult);
            uint8_t blk = TRADE_BLOCK_NONE;
            if (held <= 0) blk = TRADE_BLOCK_NO_CARGO;
            else if (station_full) blk = TRADE_BLOCK_STATION_FULL;
            /* Per-grade station count — manifest entries of (c, gi) at
             * the station — so the row reflects what's actually on the
             * shelf at this grade, not the commodity total (which would
             * read identically across all grade rows of the same item). */
            int station_grade_count =
                station_manifest_count_cg(st, (commodity_t)c, (mining_grade_t)gi);
            /* Lineage comes from the FIFO-first matching unit in the
             * SHIP manifest, but only when every unit in this row has a
             * provenance tag. In MP, any anonymous or non-wired unit is
             * reconstructed as a legacy-migrate placeholder, so mixed
             * buckets intentionally render without a lineage line. */
            uint8_t origin_idx = 0;
            uint64_t mined_blk = 0;
            bool has_lineage = false;
            const cargo_unit_t *inspect_unit = NULL;
            const cargo_receipt_chain_t *inspect_chain = NULL;
            int lineage_count = manifest_lineage_count_cg(&ship->manifest,
                                                          (commodity_t)c,
                                                          (mining_grade_t)gi);
            if (held > 0) {
                if (rep_idx >= 0) {
                    const cargo_unit_t *rep = &ship->manifest.units[rep_idx];
                    origin_idx = rep->origin_station;
                    mined_blk = rep->mined_block;
                    has_lineage = (lineage_count >= held && mined_blk != 0) ||
                                  cargo_unit_has_player_origin(rep);
                    inspect_unit = rep;
                    const ship_receipts_t *rcpts = ship_get_receipts_const(ship);
                    if (rcpts && (uint16_t)rep_idx < rcpts->count)
                        inspect_chain = &rcpts->chains[rep_idx];
                }
            }
            trade_row_t row = (trade_row_t){
                .kind = 1, .commodity = (commodity_t)c, .grade = (mining_grade_t)gi,
                .stock = held, .quantity = held > 0 ? 1 : 0,
                .unit_price = price, .total_price = price,
                .actionable = (blk == TRADE_BLOCK_NONE),
                .station_stock = station_grade_count, .station_capacity = capacity,
                .held = held, .block_reason = blk,
                .prefix_class = (uint8_t)top_cls,
                .has_lineage = has_lineage,
                .origin_station_idx = origin_idx,
                .mined_block = mined_blk,
            };
            trade_row_attach_inspect(&row, inspect_unit, inspect_chain);
            out[row_count++] = row;
        }
    }
    return row_count;
}

/* Page resolver shared by renderer + input. Pages are computed by chunking
 * the BUY block and the SELL block independently so SELL always starts on
 * a fresh page — keeps the two halves visually distinct and prevents
 * "selling on the buy page" misclicks. */
void trade_page_range(const trade_row_t *rows, int row_count,
                      int page, int *out_first, int *out_last,
                      int *out_total) {
    uint8_t kinds[TRADE_MAX_ROWS];
    int capped = row_count;
    if (capped < 0) capped = 0;
    if (capped > TRADE_MAX_ROWS) capped = TRADE_MAX_ROWS;
    for (int i = 0; i < capped; i++)
        kinds[i] = rows && rows[i].kind == 1
            ? TRADE_ROW_KIND_SELL
            : TRADE_ROW_KIND_BUY;
    trade_page_range_for_kinds(kinds, capped, TRADE_ROWS_PER_PAGE,
                               page, out_first, out_last, out_total);
}

static const contract_t *tracked_contract_for_station_ui(contract_objective_t *objective)
{
    contract_objective_t local;
    if (!objective) objective = &local;
    if (!contract_objective_for_tracked(objective)) return NULL;
    int ci = objective->contract_index;
    if (ci < 0 || ci >= MAX_CONTRACTS) return NULL;
    const contract_t *ct = &g.world.contracts[ci];
    return ct->active ? ct : NULL;
}

static bool contract_accepts_trade_row(const contract_t *ct,
                                       const trade_row_t *row)
{
    if (!ct || !row || !ct->active) return false;
    if (ct->action != CONTRACT_TRACTOR && ct->action != CONTRACT_DELIVERY)
        return false;
    if (ct->commodity < COMMODITY_RAW_ORE_COUNT) return false;
    if (ct->commodity != row->commodity) return false;
    if (row->has_inspect) {
        cargo_unit_t unit = {0};
        unit.kind = row->inspect_kind;
        unit.commodity = (uint8_t)row->commodity;
        unit.grade = (uint8_t)row->grade;
        unit.prefix_class = row->prefix_class;
        unit.origin_station = row->origin_station_idx;
        unit.quantity = row->quantity > 0
            ? (uint8_t)(row->quantity > 255 ? 255 : row->quantity)
            : 1;
        unit.recipe_id = row->inspect_recipe_id;
        unit.mined_block = row->mined_block;
        memcpy(unit.pub, row->inspect_pub, sizeof(unit.pub));
        memcpy(unit.parent_merkle, row->inspect_parent,
               sizeof(unit.parent_merkle));
        return contract_fit_is_ok(contract_fit_cargo_unit(ct, &unit));
    }
    return contract_fit_is_ok(contract_fit_cargo_fields(
        ct, row->commodity, row->grade, (uint16_t)row->quantity, false));
}

static bool contract_origin_ban_label(const contract_t *ct,
                                      char *out,
                                      size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!ct || !(ct->proof_flags & CONTRACT_PROOF_FORBID_ORIGIN) ||
        ct->forbidden_origin_mask == 0) {
        return false;
    }
    for (int i = 0; i < MAX_STATIONS && i < 64; i++) {
        if ((ct->forbidden_origin_mask & (1ULL << i)) == 0) continue;
        snprintf(out, out_size, "no %s ", station_short_name(i));
        return true;
    }
    return false;
}

static bool trade_row_tracked_note(const station_t *st,
                                   const trade_row_t *row,
                                   char *out,
                                   size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    contract_objective_t objective;
    const contract_t *ct = tracked_contract_for_station_ui(&objective);
    if (!contract_accepts_trade_row(ct, row)) return false;

    int here_idx = station_index_of(st);
    bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;

    if (ct->action == CONTRACT_DELIVERY) {
        bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_contract(objective.contract_index);
        if (row->kind == 0 && at_origin && (!ledger ||
            ledger->status == DELIVERY_SHIPMENT_DELIVERED)) {
            snprintf(out, out_size, "use jobs for credit");
            return true;
        }
        if (row->kind == 1 && row->held > 0 && at_dest && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP) {
            snprintf(out, out_size, "ready to deliver");
            return true;
        }
        return false;
    }

    if (row->kind == 0) {
        if (at_dest) return false;
        if (row->block_reason == TRADE_BLOCK_NO_FUNDS) {
            snprintf(out, out_size, "need local balance");
            return true;
        }
        if (row->station_stock > 0) {
            snprintf(out, out_size, "needed for tracked job");
            return true;
        }
        return false;
    }

    if (row->held <= 0) return false;
    if (at_dest) {
        snprintf(out, out_size, "ready to deliver");
        return true;
    }
    snprintf(out, out_size, "wrong station");
    return true;
}

static bool job_row_tracked_note(int here_idx,
                                 int contract_index,
                                 bool fulfillable_here,
                                 char *out,
                                 size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (contract_index < 0 || contract_index >= MAX_CONTRACTS) return false;
    const contract_t *ct = &g.world.contracts[contract_index];
    if (!ct->active) return false;

    bool tracked = g.tracked_contract == contract_index;
    if (ct->action == CONTRACT_DELIVERY) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_contract(contract_index);
        bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
        bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;
        int held = contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest);
        if (at_origin && ledger &&
            ledger->status == DELIVERY_SHIPMENT_DELIVERED) {
            snprintf(out, out_size, "return proof");
            return true;
        }
        if (at_origin && !ledger) {
            snprintf(out, out_size, "hail to take shipment");
            return true;
        }
        if (at_dest && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP && held > 0) {
            snprintf(out, out_size, "ready to deliver");
            return true;
        }
        if (tracked && held > 0 && !at_dest) {
            snprintf(out, out_size, "wrong station");
            return true;
        }
        if (tracked && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP) {
            snprintf(out, out_size, "deliver to destination");
            return true;
        }
    }
    if (fulfillable_here) {
        snprintf(out, out_size, "ready to deliver");
        return true;
    }
    if (!tracked) return false;

    if (ct->action == CONTRACT_TRACTOR &&
        ct->commodity >= COMMODITY_RAW_ORE_COUNT &&
        contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest) > 0 &&
        here_idx != (int)ct->station_index) {
        snprintf(out, out_size, "wrong station");
        return true;
    }

    snprintf(out, out_size, "needed for tracked job");
    return true;
}

static void draw_trade_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    float row_h = compact ? 14.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;
    const uint8_t COL_GAIN[3]  = { 130, 230, 150 };  /* + sell: green */
    const uint8_t COL_COST[3]  = { 230, 110, 110 };  /* - buy:  red   */
    const uint8_t COL_DIM[3]   = { PAL_AFFORD_INACTIVE };
    const uint8_t COL_FADED[3] = { PAL_TEXT_FADED };
    const uint8_t COL_TEXT[3]  = { PAL_TEXT_SECONDARY };
    const uint8_t COL_FLOW_OK[3]   = { 120, 220, 170 };
    const uint8_t COL_FLOW_WARN[3] = { 235, 195, 95 };
    const uint8_t COL_FLOW_BAD[3]  = { 235, 110, 110 };
    char flow_line[96] = "";
    const uint8_t *flow_rgb = NULL;

    my += draw_section_header(cx, my, inner_right, "TRADE", HDR_TRADE);

    /* Factual stock strip. Use the same buyable-market stock definition
     * as BUY rows, and include repair kits so the strip does not disagree
     * with the market below. Named ingots stay out of this count because
     * generic BUY rows deliberately skip prefix-premium collectibles. */
    {
        struct { commodity_t c; const char *code; } slots[7] = {
            { COMMODITY_FERRITE_INGOT,  "FE" },
            { COMMODITY_CUPRITE_INGOT,  "CU" },
            { COMMODITY_CRYSTAL_INGOT,  "CR" },
            { COMMODITY_FRAME,          "FM" },
            { COMMODITY_LASER_MODULE,   "LM" },
            { COMMODITY_TRACTOR_MODULE, "TM" },
            { COMMODITY_REPAIR_KIT,     "RK" },
        };
        cell_t cells[8];
        char labels[7][8];
        cells[0] = (cell_t){ 0, "STOCK", COL_FADED };
        int cell_count = 1;
        for (int i = 0; i < 7; i++) {
            commodity_t c = slots[i].c;
            int stock = 0;
            for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
                stock += station_market_stock_cg(st, c, (mining_grade_t)gi);
            snprintf(labels[i], sizeof(labels[i]), "%s%d", slots[i].code, stock);
            uint8_t cr, cg, cb;
            commodity_color_u8(c, &cr, &cg, &cb);
            static uint8_t rgb[7][3];
            rgb[i][0] = stock > 0 ? cr : 90;
            rgb[i][1] = stock > 0 ? cg : 90;
            rgb[i][2] = stock > 0 ? cb : 90;
            cells[cell_count++] = (cell_t){ 7 + i * 7, labels[i], rgb[i] };
        }
        draw_row_cells(cx, my, cells, cell_count);
        my += row_h;
    }

    {
        station_flow_summary_t summary;
        bool mirrored_authoritative = g.multiplayer_enabled && net_is_connected();
        if (station_flow_summary(st, mirrored_authoritative, &summary) &&
            station_flow_summary_format(&summary, flow_line, sizeof(flow_line))) {
            flow_rgb = COL_FLOW_OK;
            if (summary.diag == STATION_FLOW_DIAG_SLOW_FEED)
                flow_rgb = COL_FLOW_WARN;
            else if (summary.diag != STATION_FLOW_DIAG_RUNNING)
                flow_rgb = COL_FLOW_BAD;
        }
    }

    /* Single source of truth for the row list (shared with input.c so
     * a [1] keypress always hits the same row drawn here). */
    trade_row_t rows[TRADE_MAX_ROWS];
    int row_count = build_trade_rows(st, ship, rows, TRADE_MAX_ROWS);

    /* Pagination — BUY and SELL are paginated independently so SELL
     * always starts on a fresh page (see trade_page_range). [F] still
     * walks one page at a time; wraps at the last page. */
    int total_pages = 1, first = 0, last = 0;
    int page = (int)g.trade_page;
    trade_page_range(rows, row_count, page, &first, &last, &total_pages);
    if (page >= total_pages) { page = 0; g.trade_page = 0; }

    if (row_count == 0) {
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "Nothing on offer and nothing to deliver.", NULL, NULL);
        return;
    }

    /* Side indicator. BUY/SELL lives here, so individual rows can stay
     * focused on the item, stock, and price. */
    {
        char pg[32];
        pg[0] = '\0';
        if (total_pages > 1)
            snprintf(pg, sizeof(pg), "page %d/%d   [F] next",
                     page + 1, total_pages);
        const uint8_t COL_ACTIVE[3] = { 130, 210, 255 };
        const char *page_kind = (first < last && rows[first].kind == 1)
            ? "SELL" : "BUY";
        char page_left[128];
        const uint8_t *page_left_rgb = COL_ACTIVE;
        if (flow_line[0]) {
            snprintf(page_left, sizeof(page_left), "%s  %s",
                     page_kind, flow_line);
            page_left_rgb = flow_rgb ? flow_rgb : COL_ACTIVE;
        } else {
            snprintf(page_left, sizeof(page_left), "%s", page_kind);
        }
        draw_row_lr(cx, my, inner_right, page_left_rgb, page_left, COL_FADED,
                    pg[0] ? pg : NULL);
        my += row_h;
    }

    /* Hotkey numbering is by row position on the page, NOT by actionable
     * filter — a row keeps its number when it transitions blocked, so the
     * layout under the player's fingers doesn't shift mid-trade. Blocked
     * rows render their number dimmed; pressing it is a no-op. */
    for (int ri = first; ri < last; ri++) {
        const trade_row_t *r = &rows[ri];
        int slot = (ri - first) + 1;
        char key_buf[16];
        if (r->actionable) snprintf(key_buf, sizeof(key_buf), "[%d]", slot);
        else               snprintf(key_buf, sizeof(key_buf), " - ");

        const uint8_t *info_rgb = r->actionable ? COL_TEXT : COL_FADED;

        /* Grade label + tint. */
        uint8_t ggr, ggg, ggb;
        mining_grade_rgb(r->grade, &ggr, &ggg, &ggb);
        uint8_t gr_rgb[3] = { ggr, ggg, ggb };
        const char *grade_label = mining_grade_label(r->grade);
        const uint8_t *grade_rgb_ptr = r->actionable ? gr_rgb : (uint8_t*)COL_FADED;

        /* Active rows: red for buy (cost), green for sell (gain). Passive
         * rows are dimmed regardless of direction. */
        const uint8_t *total_rgb = (r->kind == 0) ? COL_COST : COL_GAIN;
        const uint8_t *row_rgb   = r->actionable ? total_rgb : COL_DIM;

        /* Status column on the left of the right-aligned price:
         * BUY:  station X/MAX
         * SELL: station X/MAX, plus held count only when the player has
         * cargo. Empty SELL rows already explain themselves in the reason
         * column, so avoid "(0 held) (none held)" duplication. */
        char status_buf[40];
        if (r->kind == 0) {
            snprintf(status_buf, sizeof(status_buf), "%d/%d",
                     r->station_stock, r->station_capacity);
        } else if (r->held > 0) {
            snprintf(status_buf, sizeof(status_buf), "%d/%d  (%d held)",
                     r->station_stock, r->station_capacity, r->held);
        } else {
            snprintf(status_buf, sizeof(status_buf), "%d/%d",
                     r->station_stock, r->station_capacity);
        }

        char total_buf[64];
        const char *trade_cur = ui_station_currency(st);
        if (r->actionable) {
            int total = r->total_price > 0 ? r->total_price : r->unit_price;
            if (r->kind == 0 && r->quantity > 1)
                snprintf(total_buf, sizeof(total_buf), "-%d %s x%d",
                         total, trade_cur, r->quantity);
            else if (r->kind == 0)
                snprintf(total_buf, sizeof(total_buf), "-%d %s", total, trade_cur);
            else
                snprintf(total_buf, sizeof(total_buf), "+%d %s", total, trade_cur);
        } else {
            const char *why = "";
            switch (r->block_reason) {
            case TRADE_BLOCK_STATION_FULL:  why = "(full)";       break;
            case TRADE_BLOCK_STATION_EMPTY: why = "(empty)";      break;
            case TRADE_BLOCK_HOLD_FULL:     why = "(hold full)";  break;
            case TRADE_BLOCK_NO_FUNDS:      why = "(no funds)";   break;
            case TRADE_BLOCK_NO_CARGO:      why = "(none held)";  break;
            default:                        why = "";             break;
            }
            snprintf(total_buf, sizeof(total_buf), "%s", why);
        }

        /* Prefix-class indicator (#prefix-pricing): drop "M-", "RATi-",
         * etc. before the commodity name so the row's premium price is
         * legible to the player. Anonymous-prefix rows render with no
         * indicator and behave like the legacy bulk path. */
        char commodity_label[32];
        const char *cname = commodity_short_name(r->commodity);
        const char *cls_prefix = NULL;
        switch ((ingot_prefix_t)r->prefix_class) {
        case INGOT_PREFIX_M:            cls_prefix = "M-"; break;
        case INGOT_PREFIX_H:            cls_prefix = "H-"; break;
        case INGOT_PREFIX_T:            cls_prefix = "T-"; break;
        case INGOT_PREFIX_S:            cls_prefix = "S-"; break;
        case INGOT_PREFIX_F:            cls_prefix = "F-"; break;
        case INGOT_PREFIX_K:            cls_prefix = "K-"; break;
        case INGOT_PREFIX_RATI:         cls_prefix = "RATi-"; break;
        case INGOT_PREFIX_COMMISSIONED: cls_prefix = "RATi*-"; break;
        case INGOT_PREFIX_ANONYMOUS:
        default:                        cls_prefix = NULL; break;
        }
        if (cls_prefix) {
            snprintf(commodity_label, sizeof(commodity_label), "%s%s", cls_prefix, cname);
        } else {
            snprintf(commodity_label, sizeof(commodity_label), "%s", cname);
        }

        /* Lineage tag — compact #344 inspection of the representative
         * cargo_unit_t behind this row. The row already names grade;
         * this line adds serial, recipe, parent summary, origin, and
         * receipt seal count where the client has those bytes locally. */
        char lineage_buf[112];
        lineage_buf[0] = '\0';
        char job_note[64];
        (void)trade_row_tracked_note(st, r, job_note, sizeof(job_note));
        if (r->has_inspect) {
            cargo_unit_t inspect = {0};
            inspect.kind = r->inspect_kind;
            inspect.commodity = (uint8_t)r->commodity;
            inspect.grade = (uint8_t)r->grade;
            inspect.recipe_id = r->inspect_recipe_id;
            inspect.origin_station = r->origin_station_idx;
            inspect.quantity = 1;
            inspect.mined_block = r->mined_block;
            memcpy(inspect.pub, r->inspect_pub, sizeof(inspect.pub));
            memcpy(inspect.parent_merkle, r->inspect_parent,
                   sizeof(inspect.parent_merkle));
            char serial[12], parent[8], origin[24];
            cargo_lineage_serial_label(&inspect, serial, sizeof(serial));
            cargo_lineage_parent_label(&inspect, parent, sizeof(parent));
            bool origin_known = inspect.recipe_id != (uint16_t)RECIPE_LEGACY_MIGRATE &&
                ((cargo_kind_t)inspect.kind == CARGO_KIND_INGOT ||
                 inspect.mined_block != 0 || r->inspect_chain_len > 0);
            if (origin_known)
                cargo_lineage_origin_label(&inspect, origin, sizeof(origin));
            else
                snprintf(origin, sizeof(origin), "origin ?");
            const char *recipe = cargo_lineage_recipe_label(&inspect);
            char seal[16] = "";
            if (r->inspect_chain_len > 0)
                snprintf(seal, sizeof(seal), " seal x%u",
                         (unsigned)r->inspect_chain_len);
            if (r->mined_block != 0) {
                snprintf(lineage_buf, sizeof(lineage_buf),
                         "serial %s  %s parent %s  %s ep%llu%s",
                         serial, recipe, parent, origin,
                         (unsigned long long)r->mined_block, seal);
            } else {
                snprintf(lineage_buf, sizeof(lineage_buf),
                         "serial %s  %s parent %s  %s%s",
                         serial, recipe, parent, origin, seal);
            }
        } else if (r->has_lineage) {
            snprintf(lineage_buf, sizeof(lineage_buf),
                     "from %s, ep %llu",
                     station_short_name((int)r->origin_station_idx),
                     (unsigned long long)r->mined_block);
        }

        if (compact) {
            cell_t top[] = {
                {  0, key_buf,                            row_rgb },
                {  4, commodity_label,                    info_rgb },
                { 22, grade_label,                        grade_rgb_ptr },
            };
            draw_row_cells(cx, my, top, 3);
            my += row_h;
            draw_row_lr(cx + 32.0f, my, inner_right,
                        info_rgb, status_buf, row_rgb, total_buf);
            my += row_h;
            if (job_note[0]) {
                char note_fit[64];
                int note_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(job_note, note_chars, note_fit, sizeof(note_fit));
                cell_t note[] = {
                    {  4, note_fit, COL_TRACKED_JOB },
                };
                draw_row_cells(cx, my, note, 1);
                my += row_h;
            }
            /* Optional third line — lineage flavor. Drawn dim so it
             * reads as background context, not actionable state. */
            if (lineage_buf[0]) {
                char lineage_fit[112];
                int lineage_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(lineage_buf, lineage_chars, lineage_fit,
                            sizeof(lineage_fit));
                cell_t lineage[] = {
                    {  4, lineage_fit, COL_FADED },
                };
                draw_row_cells(cx, my, lineage, 1);
                my += row_h;
            }
            /* Inter-row gap so two-line rows don't blur together. */
            my += 4.0f;
        } else {
            cell_t row[] = {
                {  0, key_buf,                            row_rgb },
                {  4, commodity_label,                    info_rgb },
                { 22, grade_label,                        grade_rgb_ptr },
                { 31, status_buf,                         info_rgb },
            };
            draw_row_cells(cx, my, row, 4);
            draw_row_lr(cx, my, inner_right, NULL, NULL, row_rgb, total_buf);
            my += row_h;
            if (job_note[0]) {
                char note_fit[64];
                int note_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(job_note, note_chars, note_fit, sizeof(note_fit));
                cell_t note[] = {
                    {  4, note_fit, COL_TRACKED_JOB },
                };
                draw_row_cells(cx, my, note, 1);
                my += row_h;
            }
            /* Wide mode also gets a lineage line beneath the row.
             * Same dim styling, indented under the commodity label. */
            if (lineage_buf[0]) {
                char lineage_fit[112];
                int lineage_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(lineage_buf, lineage_chars, lineage_fit,
                            sizeof(lineage_fit));
                cell_t lineage[] = {
                    {  4, lineage_fit, COL_FADED },
                };
                draw_row_cells(cx, my, lineage, 1);
                my += row_h;
            }
        }
    }
    return;
}

/* DOCK — ship bay. Two sections, both always shown:
 *   SHIP BAY — class, hull, cargo, module levels (pure state, no verbs)
 *   SERVICES — repair + per-upgrade refit rows with right-aligned status
 *
 * Row grammar: hotkey+label on the left, status/cost on the right. */
static void draw_verbs_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    float row_h = compact ? 14.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;

    const uint8_t COL_TEXT[3]  = { PAL_TEXT_SECONDARY };
    const uint8_t COL_AMBER[3] = { PAL_ORE_AMBER };
    const uint8_t COL_NAV[3]   = { PAL_NAV_BLUE };
    const uint8_t COL_DIM[3]   = { PAL_AFFORD_INACTIVE };
    const uint8_t COL_FADED[3] = { PAL_TEXT_FADED };

    if (st->scaffold) {
        /* Special case: docked at a station still being built. The "verb"
         * here is delivering frames to advance construction. */
        int pct = (int)lroundf(st->scaffold_progress * 100.0f);
        int held = ship_manifest_count_c(ship, COMMODITY_FRAME);
        char left_buf[48], right_buf[32];
        snprintf(left_buf, sizeof(left_buf), "SCAFFOLD %d%%", pct);
        draw_row_lr(cx, my, inner_right, COL_AMBER, left_buf, COL_FADED, NULL);
        my += row_h * 1.5f;
        if (held > 0) {
            snprintf(left_buf, sizeof(left_buf), "[S] deliver frames");
            snprintf(right_buf, sizeof(right_buf), "x%d -> +construction", held);
            draw_row_lr(cx, my, inner_right, COL_AMBER, left_buf, COL_TEXT, right_buf);
        } else {
            draw_row_lr(cx, my, inner_right, COL_DIM,
                        "Bring frames here to finish this outpost.", COL_TEXT, NULL);
        }
        return;
    }

    /* -------- SHIP BAY (always visible) -------- */
    my += draw_section_header(cx, my, inner_right, "SHIP BAY", HDR_FIT);
    {
        const hull_def_t *def = ship_hull_def(ship);
        const char *class_name = def && def->name ? def->name : "-";
        char right_buf[48];

        draw_row_lr(cx, my, inner_right, COL_TEXT, "class", COL_TEXT, class_name);
        my += row_h;

        snprintf(right_buf, sizeof(right_buf), "%d / %d",
                 (int)lroundf(ship->hull), (int)lroundf(ship_max_hull(ship)));
        draw_row_lr(cx, my, inner_right, COL_TEXT, "hull", COL_TEXT, right_buf);
        my += row_h;

        snprintf(right_buf, sizeof(right_buf), "%d / %d",
                 (int)lroundf(ship_manifest_backed_cargo_volume(ship)),
                 (int)lroundf(ship_cargo_capacity(ship)));
        draw_row_lr(cx, my, inner_right, COL_TEXT, "cargo", COL_TEXT, right_buf);
        /* Grade-tinted cargo fill bar -- sits inside the cargo row, just
         * below the text baseline so it visually belongs to that row.
         * Segments are sized by manifest unit volume and colored per
         * grade; raw float cargo (legacy/non-manifest ore only) is folded
         * into common grade. */
        {
            float cap_v = ship_cargo_capacity(ship);
            if (cap_v > 0.0f) {
                float bar_x  = cx + 8.0f;
                float bar_w  = inner_right - bar_x - 8.0f;
                float bar_h  = 3.0f;
                float bar_y  = my + row_h - bar_h - 2.0f;

                /* Background */
                sgl_begin_quads();
                sgl_c4f(0.10f, 0.10f, 0.12f, 0.85f);
                sgl_v2f(bar_x, bar_y);
                sgl_v2f(bar_x + bar_w, bar_y);
                sgl_v2f(bar_x + bar_w, bar_y + bar_h);
                sgl_v2f(bar_x, bar_y + bar_h);
                sgl_end();

                /* Volume per grade. */
                float vol_by_grade[MINING_GRADE_COUNT] = {0};
                for (uint16_t u = 0; u < ship->manifest.count; u++) {
                    const cargo_unit_t *cu = &ship->manifest.units[u];
                    float vol = commodity_volume((commodity_t)cu->commodity);
                    int gi = cu->grade;
                    if (gi < 0 || gi >= MINING_GRADE_COUNT) gi = MINING_GRADE_COMMON;
                    vol_by_grade[gi] += vol;
                }
                for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                    float raw_vol = ship->cargo[c] * commodity_volume((commodity_t)c);
                    if (raw_vol > 0.001f) vol_by_grade[MINING_GRADE_COMMON] += raw_vol;
                }

                /* Segments. Walk grade order so rare/RATi sit on the right. */
                float x = bar_x;
                sgl_begin_quads();
                for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
                    if (vol_by_grade[gi] < 0.001f) continue;
                    uint8_t cr, cg, cb;
                    mining_grade_rgb((mining_grade_t)gi, &cr, &cg, &cb);
                    float seg_w = bar_w * (vol_by_grade[gi] / cap_v);
                    if (seg_w < 0.0f) seg_w = 0.0f;
                    if (x + seg_w > bar_x + bar_w) seg_w = (bar_x + bar_w) - x;
                    sgl_c4f(cr / 255.0f, cg / 255.0f, cb / 255.0f, 0.95f);
                    sgl_v2f(x, bar_y);
                    sgl_v2f(x + seg_w, bar_y);
                    sgl_v2f(x + seg_w, bar_y + bar_h);
                    sgl_v2f(x, bar_y + bar_h);
                    x += seg_w;
                }
                sgl_end();
            }
        }
        my += row_h;

        snprintf(right_buf, sizeof(right_buf), "LSR %d  HLD %d  TRC %d",
                 ship->mining_level, ship->hold_level, ship->tractor_level);
        draw_row_lr(cx, my, inner_right, COL_TEXT, "mods", COL_TEXT, right_buf);
        my += row_h;
    }
    my += 6.0f;

    /* -------- SERVICES (always visible; rows always show their status) -------- */
    my += draw_section_header(cx, my, inner_right, "SERVICES", HDR_SERVICE);

    /* [R] repair hull — same grammar as the upgrade rows:
     *   kits [ have / need ]  -N cr
     * "have" = ship cargo + dock inventory; "need" = HP missing (1
     * kit per HP). Append the credit cost when actionable. */
    {
        int kits_avail = ui->ship_kits + ui->station_kits;
        int kits_needed = ui->hull_max - ui->hull_now;
        if (kits_needed < 0) kits_needed = 0;
        char right_buf[32];
        char short_cur[5];
        ui_station_currency_short(st, short_cur, sizeof(short_cur));
        if (ui->hull_now >= ui->hull_max) {
            draw_row_lr(cx, my, inner_right, COL_DIM, "hull",
                        COL_FADED, "full");
        } else if (kits_avail <= 0) {
            /* Kits gate the repair regardless of credits — surface the
             * shortfall before the credit cost so the [R] row matches
             * what the action handler will say if pressed. */
            int n = kits_needed > 0 ? kits_needed : 1;
            snprintf(right_buf, sizeof(right_buf), "%d repair kit%s needed",
                     n, n == 1 ? "" : "s");
            draw_row_lr(cx, my, inner_right, COL_DIM, "repair hull",
                        COL_FADED, right_buf);
        } else if (ui->can_repair && ui->repair_cost > 0) {
            snprintf(right_buf, sizeof(right_buf), "%d %s",
                     ui->repair_cost, short_cur);
            draw_row_lr(cx, my, inner_right, COL_AMBER, "[R] repair hull",
                        COL_TEXT, right_buf);
        } else if (ui->repair_cost > 0) {
            snprintf(right_buf, sizeof(right_buf), "%d %s needed",
                     ui->repair_cost, short_cur);
            draw_row_lr(cx, my, inner_right, COL_DIM, "repair",
                        COL_FADED, right_buf);
        }
        my += row_h;
    }

    /* [M] tune laser, [C] expand hold, [T] tune tractor — same grammar.
     * Real cost is the modules themselves (frames / lasers / tractors
     * pulled from cargo). If cargo is short, the dock fills the gap
     * from its own inventory at retail. Any dock can install — module
     * supply is the only gate. */
    struct { const char *left; const char *passive_left;
             const char *unit_singular; const char *unit_plural;
             int needed, in_cargo, at_station, credit; bool can; bool maxed; } refit[3] = {
        { "[M] tune laser",   "laser",   "laser module",   "laser modules",
          ui->mining_units_needed, ui->mining_units_in_cargo,
          ui->mining_units_at_station, ui->mining_credit_cost,
          ui->can_upgrade_mining,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING) },
        { "[C] expand hold",  "hold",    "frame", "frames",
          ui->hold_units_needed, ui->hold_units_in_cargo,
          ui->hold_units_at_station, ui->hold_credit_cost,
          ui->can_upgrade_hold,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_HOLD) },
        { "[T] tune tractor", "tractor", "tractor module", "tractor modules",
          ui->tractor_units_needed, ui->tractor_units_in_cargo,
          ui->tractor_units_at_station, ui->tractor_credit_cost,
          ui->can_upgrade_tractor,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_TRACTOR) },
    };
    for (int i = 0; i < 3; i++) {
        char right_buf[40];
        char short_cur[5];
        ui_station_currency_short(st, short_cur, sizeof(short_cur));
        int avail  = refit[i].in_cargo + refit[i].at_station;
        int needed = refit[i].needed;
        const char *plural = refit[i].unit_plural;
        if (refit[i].can) {
            /* Actionable: show the hotkey + verb with cost. */
            if (refit[i].credit > 0)
                snprintf(right_buf, sizeof(right_buf), "%d %s",
                         refit[i].credit, short_cur);
            else
                snprintf(right_buf, sizeof(right_buf), "ready");
            draw_row_lr(cx, my, inner_right, COL_NAV, refit[i].left,
                        COL_TEXT, right_buf);
            my += row_h;
            continue;
        }
        /* Non-actionable: hide the [verb] and keep the left label short;
         * the right side carries the concrete missing material. */
        const char *short_label = refit[i].passive_left;
        if (refit[i].maxed) {
            snprintf(right_buf, sizeof(right_buf), "maxed");
        } else if (avail <= 0) {
            int short_by = needed - avail;
            if (short_by <= 0) short_by = needed > 0 ? needed : 1;
            snprintf(right_buf, sizeof(right_buf), "need %d %s",
                     short_by, short_by == 1
                         ? refit[i].unit_singular : refit[i].unit_plural);
        } else if (refit[i].credit > 0) {
            snprintf(right_buf, sizeof(right_buf), "need %d %s",
                     refit[i].credit, short_cur);
        } else {
            snprintf(right_buf, sizeof(right_buf), "unavailable");
        }
        draw_row_lr(cx, my, inner_right, COL_DIM, short_label,
                    COL_FADED, right_buf);
        my += row_h;
        (void)plural;
    }
}

/* ------------------------------------------------------------------ */
/* JOBS sub-screen — preserved CONTRACTS picker, trimmed              */
/* ------------------------------------------------------------------ */

/* WORK view — dispatch board table.
 * Columns (monospace cells, 8px each):
 *   key(4) job(10) cargo(19) state(10)  payout(right-aligned)
 * Rows are sorted: fulfillable here first, then nearest remaining. */
static void draw_jobs_view(const station_ui_state_t *ui,
                           float cx, float cy, float inner_w, bool compact)
{
    (void)compact;
    (void)inner_w;
    float row_h = compact ? 14.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;

    const uint8_t COL_HDR[3]      = { PAL_TEXT_FADED };
    const uint8_t COL_READY[3]    = { PAL_CONTRACT_READY };
    const uint8_t COL_STATUS[3]   = { PAL_CONTRACT_STATUS };
    const uint8_t COL_SELECTED[3] = { PAL_CONTRACT_PENDING };
    const uint8_t COL_DIM[3]      = { PAL_CONTRACT_HINT };
    const uint8_t COL_TEXT[3]     = { PAL_TEXT_SECONDARY };
    const uint8_t COL_FADED[3]    = { PAL_TEXT_FADED };

    my += draw_section_header(cx, my, inner_right, "JOBS", HDR_TRADE);

    /* Column header: job verb is now the state (fracture / tractor /
     * deliver), payout left-aligned at col 33 so it reads together with
     * the cargo instead of fighting it at the panel edge. */
    if (!compact) {
        cell_t hdr[] = {
            {  0, "key",    COL_HDR },
            {  4, "job",    COL_HDR },
            { 14, "cargo",  COL_HDR },
            { 33, "payout", COL_HDR },
        };
        draw_row_cells(cx, my, hdr, 4);
        my += row_h;
    }

    /* Build slot listing via the shared helper so the rows the player
     * sees here are exactly the rows [1]/[2]/[3] selects from in
     * input.c — no duplication, no drift. */
    int slots[3] = {-1, -1, -1};
    bool slot_fulfillable[3] = {false, false, false};
    int slot_held[3] = {0, 0, 0};
    int here_idx = LOCAL_PLAYER.current_station;
    int slot_count = build_work_slots(here_idx, ui->station->pos,
                                      slots, slot_fulfillable, slot_held);

    if (slot_count == 0) {
        draw_row_lr(cx, my, inner_right, COL_DIM, "No active contracts.", NULL, NULL);
        return;
    }

    for (int s = 0; s < slot_count; s++) {
        contract_t *ct = &g.world.contracts[slots[s]];
        float cprice = ct->base_price * (1.0f + fminf(ct->age / 300.0f, 1.0f) * 0.2f);
        bool tracked = (g.tracked_contract == slots[s]);
        bool selected = (g.selected_contract == slots[s]);

        const uint8_t *row_rgb;
        if (selected)               row_rgb = COL_SELECTED;
        else if (slot_fulfillable[s]) row_rgb = COL_READY;
        else if (tracked)            row_rgb = COL_STATUS;
        else                         row_rgb = COL_DIM;

        /* Selected row accent bar (left edge). */
        if (selected) {
            sgl_begin_quads();
            sgl_c4f(row_rgb[0] / 255.0f, row_rgb[1] / 255.0f, row_rgb[2] / 255.0f, 0.95f);
            sgl_v2f(cx - 10.0f, my - 2.0f);
            sgl_v2f(cx - 7.0f,  my - 2.0f);
            sgl_v2f(cx - 7.0f,  my + 14.0f);
            sgl_v2f(cx - 10.0f, my + 14.0f);
            sgl_end();
        }

        char key_buf[8], cargo_buf[32], pay_buf[64]; /* 64 = room for "+%d %s" with 31-char currency name */
        snprintf(key_buf, sizeof(key_buf), "[%d]%s",
                 s + 1, tracked && !selected ? "*" : "");

        /* Job column doubles as the immediate next-step verb. The shared
         * objective resolver also drives SIGNAL copy and world markers, so
         * the station board cannot drift from the HUD hint. */
        const char *job_txt = "work";
        contract_objective_t objective;
        if (contract_objective_for_contract(slots[s], &objective) &&
            objective.job[0] != '\0') {
            job_txt = objective.job;
        }

        const station_t *dest = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;

        if (ct->action == CONTRACT_FRACTURE) {
            snprintf(cargo_buf, sizeof(cargo_buf), "asteroid field");
        } else if (objective.kind == CONTRACT_OBJECTIVE_PICKUP &&
                   objective.source_station >= 0 &&
                   objective.target_station >= 0) {
            char source_name[12], dest_name[12];
            char route_buf[32];
            ui_station_name_short(objective.source_station,
                                  source_name, sizeof(source_name));
            ui_station_name_short(objective.target_station,
                                  dest_name, sizeof(dest_name));
            snprintf(route_buf, sizeof(route_buf), "%s %s>%s",
                     commodity_short_name(ct->commodity),
                     source_name, dest_name);
            ui_fit_text(route_buf, 18, cargo_buf, sizeof(cargo_buf));
        } else {
            int qty = slot_fulfillable[s] ? slot_held[s]
                                          : (int)lroundf(ct->quantity_needed);
            /* Drop the grade word — color encodes rarity for the whole
             * row (see the grade-tint override below). Keeps the cargo
             * cell short so payout doesn't collide with it. */
            char req_prefix[32];
            if (!contract_origin_ban_label(ct, req_prefix, sizeof(req_prefix)))
                snprintf(req_prefix, sizeof(req_prefix), "%s",
                         ct->proof_flags ? "trace " : "");
            snprintf(cargo_buf, sizeof(cargo_buf), "%s%s x%d",
                     req_prefix,
                     commodity_short_name(ct->commodity), qty);
        }

        /* Currency fallback chain: destination station's currency (that's
         * where the payout actually lands), then the current station's
         * (outpost destinations have empty currency_name today), then
         * "cr" if both are empty. */
        const char *pay_cur = (dest && dest->currency_name[0])
            ? dest->currency_name
            : (ui->station && ui->station->currency_name[0]
               ? ui->station->currency_name : "cr");
        if (!isfinite(cprice) || cprice < 0.0f || cprice > 1.0e7f) {
            snprintf(pay_buf, sizeof(pay_buf), "+??? %s", pay_cur);
        } else {
            snprintf(pay_buf, sizeof(pay_buf), "+%d %s",
                     (int)lroundf(cprice), pay_cur);
        }

        const uint8_t *info_rgb = (row_rgb == COL_DIM) ? COL_FADED : COL_TEXT;
        /* Grade tint for the entire row when the contract is for a
         * rare grade. Common grade keeps the neutral row color so the
         * board doesn't constantly look "lit up". Selected/tracked
         * states already override row_rgb above, so those states win
         * over the grade tint (color is the action signal first,
         * rarity second). */
        uint8_t ggr, ggg, ggb;
        mining_grade_rgb((mining_grade_t)ct->required_grade, &ggr, &ggg, &ggb);
        uint8_t grade_rgb[3] = { ggr, ggg, ggb };
        bool rare_grade = ct->action != CONTRACT_FRACTURE
                       && ct->required_grade > (uint8_t)MINING_GRADE_COMMON;
        if (rare_grade && !selected && !tracked) {
            row_rgb = grade_rgb;
        }
        const uint8_t *cargo_rgb = (ct->action == CONTRACT_FRACTURE)
            ? info_rgb
            : (rare_grade ? grade_rgb : info_rgb);
        if (compact) {
            cell_t top[] = {
                {  0, key_buf,   row_rgb },
                {  4, job_txt,   row_rgb },
                { 14, cargo_buf, cargo_rgb },
            };
            draw_row_cells(cx, my, top, 3);
            my += row_h;
            cell_t bot[] = {
                { 14, pay_buf, row_rgb },
            };
            draw_row_cells(cx, my, bot, 1);
            my += row_h;
            char job_note[64];
            if (job_row_tracked_note(here_idx, slots[s],
                                     slot_fulfillable[s],
                                     job_note, sizeof(job_note))) {
                cell_t note[] = {
                    { 14, job_note, COL_TRACKED_JOB },
                };
                draw_row_cells(cx, my, note, 1);
                my += row_h;
            }
            /* Group separator between multi-line rows. Without it the
             * pay line of row N hugs the key line of row N+1 visually
             * because they share the same row_h spacing. */
            my += 4.0f;
        } else {
            cell_t row[] = {
                {  0, key_buf,   row_rgb },
                {  4, job_txt,   row_rgb },
                { 14, cargo_buf, cargo_rgb },
                { 33, pay_buf,   row_rgb },
            };
            draw_row_cells(cx, my, row, 4);
            my += row_h;
            char job_note[64];
            if (job_row_tracked_note(here_idx, slots[s],
                                     slot_fulfillable[s],
                                     job_note, sizeof(job_note))) {
                char note_fit[64];
                int note_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(job_note, note_chars, note_fit, sizeof(note_fit));
                cell_t note[] = {
                    {  4, note_fit, COL_TRACKED_JOB },
                };
                draw_row_cells(cx, my, note, 1);
                my += row_h;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* YARD view — fabrication tab: KITS catalog + construction QUEUE       */
/* ------------------------------------------------------------------ */

static void draw_yard_view(const station_ui_state_t *ui,
                           float cx, float cy, float inner_w, bool compact)
{
    (void)compact;
    const station_t *st = ui->station;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;

    if (!station_has_module(st, MODULE_SHIPYARD)) {
        float row_h = compact ? 14.0f : 15.0f;
        sdtx_color3b(PAL_SHIPYARD_HINT);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_puts("No shipyard installed at this station.");
        my += row_h;
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_puts("Visit or build a station with a shipyard module to");
        my += row_h;
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_puts("fabricate kits.");
        return;
    }

    /* -------- KITS -------- */
    my += draw_section_header(cx, my, inner_right, "KITS", HDR_YARD);
    sdtx_color3b(PAL_STATION_HINT);
    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
    sdtx_puts("[1-9] order a scaffold kit");

    float ly = my + 16.0f;
    int credits = (int)lroundf(player_current_balance());
    int shown = 0;
    int locked = 0;
    bool any = false;
    for (int t = 0; t < MODULE_COUNT && shown < 9; t++) {
        module_type_t kit = (module_type_t)t;
        if (module_kind(kit) == MODULE_KIND_NONE) continue;
        if (!station_has_module(ui->station, kit)) continue;
        any = true;
        bool unlocked = module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules, kit);
        if (!unlocked) { locked++; continue; }
        int fee = scaffold_order_fee(kit);
        int mat = (int)module_build_cost_lookup(kit);
        commodity_t mat_type = module_build_material_lookup(kit);
        const char *mat_name = commodity_short_label(mat_type);
        bool can_afford = credits >= fee;
        sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
        if (can_afford) sdtx_color3b(PAL_TEXT_SECONDARY);
        else            sdtx_color3b(PAL_CANNOT_AFFORD);
        if (compact) {
            char short_cur[8];
            ui_station_currency_short(ui->station, short_cur, sizeof(short_cur));
            sdtx_printf("[%d] %-14s %d %s + %d %s",
                shown + 1, module_type_name(kit), fee, short_cur, mat, mat_name);
        } else {
            sdtx_printf("[%d] %-14s %d %s + %d %s",
                shown + 1, module_type_name(kit),
                fee, ui_station_currency(ui->station),
                mat, mat_name);
        }
        shown++;
        ly += 14.0f;
    }
    if (!any) {
        sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
        sdtx_color3b(PAL_SHIPYARD_HINT);
        sdtx_puts("This yard has no production lines installed.");
        ly += 14.0f;
    }
    if (locked > 0) {
        sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
        sdtx_color3b(PAL_AFFORD_INACTIVE);
        sdtx_printf("+%d more locked (build prerequisites first)", locked);
        ly += 14.0f;
    }

    /* -------- QUEUE — pending construction orders -------- */
    if (ui->station->pending_scaffold_count > 0) {
        ly += 10.0f;
        ly += draw_section_header(cx, ly, inner_right, "QUEUE", HDR_YARD);
        const scaffold_t *nascent = NULL;
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            if (g.world.scaffolds[i].active &&
                g.world.scaffolds[i].state == SCAFFOLD_NASCENT) {
                for (int s = 0; s < MAX_STATIONS; s++) {
                    if (&g.world.stations[s] == ui->station &&
                        g.world.scaffolds[i].built_at_station == s) {
                        nascent = &g.world.scaffolds[i];
                        break;
                    }
                }
                if (nascent) break;
            }
        }
        for (int p = 0; p < ui->station->pending_scaffold_count; p++) {
            module_type_t t = ui->station->pending_scaffolds[p].type;
            commodity_t mat_type = module_build_material_lookup(t);
            float need = module_build_cost_lookup(t);
            float have = (p == 0 && nascent) ? nascent->build_amount : 0.0f;
            float station_have = (mat_type >= COMMODITY_RAW_ORE_COUNT)
                ? (float)station_manifest_count_c(ui->station, mat_type)
                : ui->station->_inventory_cache[mat_type];
            int got = (int)lroundf(have);
            int total = (int)lroundf(need);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
            if (p == 0) {
                sdtx_color3b(PAL_DELIVERY_BLUE);
                sdtx_printf("  %d. %s  intake %d/%d  (stock: %d)",
                    p + 1, module_type_name(t), got, total, (int)lroundf(station_have));
            } else {
                sdtx_color3b(PAL_SUPPLY_DIM);
                sdtx_printf("  %d. %s  queued", p + 1, module_type_name(t));
            }
            ly += 14.0f;
        }
    }
}

static bool station_panel_visible_always(const station_t *station)
{
    (void)station;
    return true;
}

static bool station_panel_visible_shipyard(const station_t *station)
{
    return station && station_has_module(station, MODULE_SHIPYARD);
}

static const station_panel_descriptor_t STATION_PANELS[STATION_VIEW_COUNT] = {
    [STATION_VIEW_DOCK] = {
        .view = STATION_VIEW_DOCK,
        .label = "SHIP",
        .visible_fn = station_panel_visible_always,
        .draw_fn = draw_verbs_view,
        .input_fn = station_panel_input_dock,
    },
    [STATION_VIEW_TRADE] = {
        .view = STATION_VIEW_TRADE,
        .label = "TRADE",
        .visible_fn = station_panel_visible_always,
        .draw_fn = draw_trade_view,
        .input_fn = station_panel_input_trade,
    },
    [STATION_VIEW_WORK] = {
        .view = STATION_VIEW_WORK,
        .label = "WORK",
        .visible_fn = station_panel_visible_always,
        .draw_fn = draw_jobs_view,
        .input_fn = station_panel_input_work,
    },
    [STATION_VIEW_YARD] = {
        .view = STATION_VIEW_YARD,
        .label = "YARD",
        .visible_fn = station_panel_visible_shipyard,
        .draw_fn = draw_yard_view,
        .input_fn = station_panel_input_yard,
    },
};

const station_panel_descriptor_t *station_panel_descriptor(station_view_t view)
{
    int index = (int)view;
    if (index < 0 || index >= (int)STATION_VIEW_COUNT) return NULL;
    const station_panel_descriptor_t *panel = &STATION_PANELS[index];
    return panel->label ? panel : NULL;
}

bool station_panel_visible(const station_panel_descriptor_t *panel,
                           const station_t *station)
{
    if (!panel) return false;
    return !panel->visible_fn || panel->visible_fn(station);
}

int station_panel_visible_count(const station_t *station)
{
    int count = 0;
    for (int i = 0; i < (int)STATION_VIEW_COUNT; i++) {
        const station_panel_descriptor_t *panel =
            station_panel_descriptor((station_view_t)i);
        if (station_panel_visible(panel, station)) count++;
    }
    return count;
}

const station_panel_descriptor_t *station_panel_visible_at(
    const station_t *station, int visible_index)
{
    if (visible_index < 0) return NULL;
    int count = 0;
    for (int i = 0; i < (int)STATION_VIEW_COUNT; i++) {
        const station_panel_descriptor_t *panel =
            station_panel_descriptor((station_view_t)i);
        if (!station_panel_visible(panel, station)) continue;
        if (count == visible_index) return panel;
        count++;
    }
    return NULL;
}

station_view_t station_panel_first_visible(const station_t *station)
{
    const station_panel_descriptor_t *panel =
        station_panel_visible_at(station, 0);
    return panel ? panel->view : STATION_VIEW_DOCK;
}

station_view_t station_panel_next_visible(station_view_t current,
                                          const station_t *station,
                                          int direction)
{
    int count = station_panel_visible_count(station);
    if (count <= 0) return STATION_VIEW_DOCK;

    int current_visible = -1;
    for (int i = 0; i < count; i++) {
        const station_panel_descriptor_t *panel =
            station_panel_visible_at(station, i);
        if (panel && panel->view == current) {
            current_visible = i;
            break;
        }
    }

    if (current_visible < 0)
        return station_panel_first_visible(station);

    int delta = direction < 0 ? -1 : 1;
    int next = (current_visible + delta + count) % count;
    const station_panel_descriptor_t *panel =
        station_panel_visible_at(station, next);
    return panel ? panel->view : STATION_VIEW_DOCK;
}

void station_panel_sample_current(input_intent_t *intent)
{
    if (!LOCAL_PLAYER.docked) return;
    const station_t *station = current_station_ptr();
    const station_panel_descriptor_t *panel =
        station_panel_descriptor(g.station_view);
    if (!station_panel_visible(panel, station)) {
        g.station_view = station_panel_first_visible(station);
        panel = station_panel_descriptor(g.station_view);
    }
    if (panel && panel->input_fn) panel->input_fn(intent);
}

/* ------------------------------------------------------------------ */
/* draw_station_services -- header band + view dispatch                */
/* ------------------------------------------------------------------ */

void draw_station_services(const station_ui_state_t* ui) {
    if (!LOCAL_PLAYER.docked) return;
    if (!ui->station) return;

    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    bool compact = ui_is_compact();

    draw_header_band(ui, panel_x, panel_y, panel_w, compact);

    /* View content begins below the 3-line header (last line at panel_y+58)
     * and the divider rule at panel_y+72. */
    float inner_x = panel_x + 18.0f;
    float inner_w = panel_w - 36.0f;
    float content_top = panel_y + 78.0f;
    float cx = inner_x + 18.0f;

    /* Station-role tint — used sparingly: active tab latch + section rules
     * elsewhere. Not washed across the whole panel. */
    float rr = 1, rg = 1, rb = 1;
    station_role_color(ui->station, &rr, &rg, &rb);
    const station_panel_descriptor_t *active_panel =
        station_panel_descriptor(g.station_view);
    if (!station_panel_visible(active_panel, ui->station)) {
        g.station_view = station_panel_first_visible(ui->station);
        active_panel = station_panel_descriptor(g.station_view);
    }

    /* Tab strip, LEFT-aligned on the first content line.
     *   SHIP  — ship bay (repair / refit / current ship state)
     *   TRADE — market (buy / sell cargo)
     *   WORK  — contracts (jobs / routing)
     *   YARD  — fabrication (kits + construction queue, shipyard stations only)
     * Active tab: station-role tint + a short underline latch. Inactive:
     * muted. The nav legend sits flush against the panel right edge. */
    {
        const float cell_w = 8.0f;
        float ty = content_top + 2.0f;
        float tx = cx;
        /* Record active tab geometry so we can draw the latch after the
         * text pass (so the quad doesn't clobber glyphs). */
        float active_x0 = 0.0f, active_x1 = 0.0f;
        bool active_seen = false;
        int panel_count = station_panel_visible_count(ui->station);
        for (int i = 0; i < panel_count; i++) {
            const station_panel_descriptor_t *panel =
                station_panel_visible_at(ui->station, i);
            if (!panel) continue;
            bool active = (panel->view == g.station_view);
            char cell[20];
            snprintf(cell, sizeof(cell), active ? "[%s]" : " %s ",
                     panel->label);
            float w = (float)strlen(cell) * cell_w;
            if (active) {
                sdtx_color3b((uint8_t)(rr * 255.0f),
                             (uint8_t)(rg * 255.0f),
                             (uint8_t)(rb * 255.0f));
                active_x0 = tx;
                active_x1 = tx + w;
                active_seen = true;
            } else {
                sdtx_color3b(PAL_TEXT_MUTED);
            }
            sdtx_pos(ui_text_pos(tx), ui_text_pos(ty));
            sdtx_puts(cell);
            tx += w + 10.0f;
        }
        const char *hint = "[TAB] cycle";
        float hint_w = (float)strlen(hint) * cell_w;
        float hint_x = panel_x + panel_w - 20.0f - hint_w;
        if (tx + 12.0f < hint_x) {
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(hint_x), ui_text_pos(ty));
            sdtx_puts(hint);
        }

        /* Latch underline beneath active tab — diegetic "channel selected"
         * affordance in the station-role tint. */
        if (active_seen) {
            float uy = ty + 14.0f;
            sgl_begin_quads();
            sgl_c4f(rr, rg, rb, 0.9f);
            sgl_v2f(active_x0, uy);
            sgl_v2f(active_x1, uy);
            sgl_v2f(active_x1, uy + 2.0f);
            sgl_v2f(active_x0, uy + 2.0f);
            sgl_end();
        }
    }
    float cy = content_top + 34.0f;

    if (active_panel && active_panel->draw_fn)
        active_panel->draw_fn(ui, cx, cy, inner_w, compact);

    (void)panel_h;
}
