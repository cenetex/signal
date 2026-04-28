/*
 * station_ui.c -- Station lookup helpers, formatting, and the docked station
 * services text renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "palette.h"
#include "mining_client.h"
#include "manifest.h"
/* Grade palette lives in shared/mining.h (pulled in via client.h →
 * types.h → mining.h) alongside the grade enum + label + multiplier. */

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

int nearest_signal_station(vec2 pos) {
    float best = 0.0f;
    int best_idx = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_provides_signal(st)) continue;
        float dist = sqrtf(v2_dist_sq(pos, st->pos));
        if (dist > st->signal_range) continue;
        float strength = 1.0f - dist / st->signal_range;
        if (strength > best) { best = strength; best_idx = s; }
    }
    return (best_idx >= 0) ? best_idx : nearest_station_index(pos);
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

const char* station_role_market_title(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "SMELTER";
        case MODULE_FRAME_PRESS: return "FRAME BAY";
        case MODULE_LASER_FAB:   return "FIELD BENCH";
        case MODULE_TRACTOR_FAB: return "FIELD BENCH";
        case MODULE_SIGNAL_RELAY:return "OUTPOST";
        default:                 return "MARKET";
    }
}

const char* station_role_fit_title(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "HAUL";
        case MODULE_FRAME_PRESS: return "FIT";
        case MODULE_LASER_FAB:   return "TUNING";
        case MODULE_TRACTOR_FAB: return "TUNING";
        case MODULE_SIGNAL_RELAY:return "OUTPOST";
        default:                 return "STATUS";
    }
}

void station_role_color(const station_t* station, float* r, float* g0, float* b) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     PAL_UNPACK3(PAL_MODULE_FURNACE,     *r, *g0, *b); break;
        case MODULE_FURNACE_CU:  PAL_UNPACK3(PAL_MODULE_FURNACE_CU,  *r, *g0, *b); break;
        case MODULE_FURNACE_CR:  PAL_UNPACK3(PAL_MODULE_FURNACE_CR,  *r, *g0, *b); break;
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

void format_ingot_stock_line(const station_t* station, char* text, size_t text_size) {
    int fe = (int)lroundf(station_inventory_amount(station, COMMODITY_FERRITE_INGOT));
    int cu = (int)lroundf(station_inventory_amount(station, COMMODITY_CUPRITE_INGOT));
    int cr = (int)lroundf(station_inventory_amount(station, COMMODITY_CRYSTAL_INGOT));
    /* Use full short names so the player can parse the line without
     * memorising 2-letter ingot codes (FR/CO/LN are non-obvious). */
    snprintf(text, text_size, "%s %d  %s %d  %s %d",
        commodity_short_name(COMMODITY_FERRITE_INGOT), fe,
        commodity_short_name(COMMODITY_CUPRITE_INGOT), cu,
        commodity_short_name(COMMODITY_CRYSTAL_INGOT), cr);
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

    /* Pass 1: TRACTOR contracts at this station the player can fulfill
     * right now. Raw ore lives in towed S-tier fragments rather than
     * ship.cargo[]; finished goods live on ship.cargo[]. */
    for (int ci = 0; ci < MAX_CONTRACTS && count < 3; ci++) {
        const contract_t *ct = &g.world.contracts[ci];
        if (!ct->active) continue;
        if (ct->action != CONTRACT_TRACTOR) continue;
        if (here_idx < 0 || ct->station_index != here_idx) continue;
        int held_int = 0;
        if (ct->commodity < COMMODITY_RAW_ORE_COUNT) {
            float held_ore = 0.0f;
            const ship_t *ship = &LOCAL_PLAYER.ship;
            for (int t = 0; t < ship->towed_count; t++) {
                int fi = ship->towed_fragments[t];
                if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
                const asteroid_t *a = &g.world.asteroids[fi];
                if (!a->active || a->tier != ASTEROID_TIER_S) continue;
                if (a->commodity != ct->commodity) continue;
                held_ore += a->ore;
            }
            held_int = (int)lroundf(held_ore);
        } else {
            held_int = (int)lroundf(LOCAL_PLAYER.ship.cargo[ct->commodity]);
        }
        if (held_int <= 0) continue;
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
            const contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->station_index >= MAX_STATIONS) continue;
            if (!station_exists(&g.world.stations[ct->station_index])) continue;
            bool already = false;
            for (int s = 0; s < count; s++)
                if (out_contracts[s] == ci) { already = true; break; }
            if (already) continue;
            vec2 target = (ct->action == CONTRACT_TRACTOR)
                ? g.world.stations[ct->station_index].pos : ct->target_pos;
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

    /* Compute per-upgrade module accounting (cargo first, dock fallback). */
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
        int in_cargo  = (int)floorf(LOCAL_PLAYER.ship.cargo[c] + 0.0001f);
        int at_station = ui->station
            ? (int)floorf(ui->station->inventory[c] + 0.0001f) : 0;
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
    ui->ship_kits    = (int)floorf(LOCAL_PLAYER.ship.cargo[COMMODITY_REPAIR_KIT]
                                   + 0.0001f);
    ui->station_kits = (ui->station)
        ? (int)floorf(ui->station->inventory[COMMODITY_REPAIR_KIT] + 0.0001f)
        : 0;
    int hp_needed = ui->hull_max - ui->hull_now;
    if (hp_needed < 0) hp_needed = 0;
    int kits_avail = ui->ship_kits + ui->station_kits;
    ui->kits_short_by = (hp_needed > kits_avail) ? (hp_needed - kits_avail) : 0;
    float bal = player_current_balance();
    ui->can_upgrade_mining = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_MINING, bal);
    ui->can_upgrade_hold = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD, bal);
    ui->can_upgrade_tractor = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_TRACTOR, bal);
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
    (void)compact;
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

    /* Line 1: station name (left)  ·  [E] LAUNCH (right) */
    sdtx_color3b(PAL_TEXT_PRIMARY);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L1));
    sdtx_puts(st->name);

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
    float role_w = (float)strlen(role_label) * cell_w;
    sdtx_color3b(PAL_HOLD_CYAN);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L2));
    sdtx_puts(role_label);

    if (panel_w >= 360.0f) {
        int balance = (int)lroundf(player_current_balance());
        float sig = signal_strength_at(&g.world, st->pos);
        float gap = 16.0f;
        float right_limit = panel_x + panel_w - right_margin;
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
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L3));
            sdtx_puts(line);
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
    if (left_txt && left_txt[0]) {
        sdtx_color3b(left_rgb[0], left_rgb[1], left_rgb[2]);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_puts(left_txt);
    }
    if (right_txt && right_txt[0]) {
        float rw = (float)strlen(right_txt) * cell_w;
        sdtx_color3b(right_rgb[0], right_rgb[1], right_rgb[2]);
        sdtx_pos(ui_text_pos(inner_right - rw), ui_text_pos(my));
        sdtx_puts(right_txt);
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

/* station_manifest_has_commodity / ship_manifest_has_commodity removed —
 * after the manifest-first TRADE rewrite the rows always probe the
 * full grade range and add an unknown-origin row when inventory >
 * manifest_total, so the "any-grade?" predicate is no longer needed. */

/* Ship manifest helpers — iterate directly. Ship cargo isn't broadcast
 * grade-grouped over the wire yet (only the local player's manifest
 * lives on the local client regardless of SP/MP, via LOCAL_PLAYER). */
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
    int row_count = 0;
    float free_volume = ship_cargo_capacity(ship) - ship_total_cargo(ship);
    float credits = player_current_balance();
    int capacity = (int)lroundf(MAX_PRODUCT_STOCK);

    /* BUY rows -- one per (commodity, grade) the station produces.
     * Empty grades fold under "common" so the player still sees the
     * commodity line as part of the market when stock is zero. */
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT && row_count < max; c++) {
        if (!station_produces(st, (commodity_t)c)) continue;
        float price_base = station_sell_price(st, (commodity_t)c);
        if (price_base <= FLOAT_EPSILON) continue;
        int station_inv = (int)lroundf(st->inventory[c]);
        bool emitted_any_grade = false;
        for (int gi = 0; gi < MINING_GRADE_COUNT && row_count < max; gi++) {
            int stock = station_manifest_count_cg(st, (commodity_t)c, (mining_grade_t)gi);
            if (stock <= 0) continue;
            emitted_any_grade = true;
            int price = (int)lroundf(price_base
                    * mining_payout_multiplier((mining_grade_t)gi));
            float vol = commodity_volume((commodity_t)c);
            bool has_volume = (free_volume + FLOAT_EPSILON >= vol);
            bool has_funds  = (credits >= (float)price);
            uint8_t blk = TRADE_BLOCK_NONE;
            if (!has_volume) blk = TRADE_BLOCK_HOLD_FULL;
            else if (!has_funds) blk = TRADE_BLOCK_NO_FUNDS;
            out[row_count++] = (trade_row_t){
                .kind = 0, .commodity = (commodity_t)c, .grade = (mining_grade_t)gi,
                .stock = stock, .unit_price = price,
                .actionable = (blk == TRADE_BLOCK_NONE), .is_float_fallback = false,
                .station_stock = station_inv, .station_capacity = capacity,
                .held = 0, .block_reason = blk,
            };
        }
        /* Station produces this commodity but every grade is empty.
         * Surface a passive "empty" row so the market line is visible. */
        if (!emitted_any_grade && row_count < max) {
            out[row_count++] = (trade_row_t){
                .kind = 0, .commodity = (commodity_t)c, .grade = MINING_GRADE_COMMON,
                .stock = 0, .unit_price = (int)lroundf(price_base),
                .actionable = false, .is_float_fallback = false,
                .station_stock = station_inv, .station_capacity = capacity,
                .held = 0, .block_reason = TRADE_BLOCK_STATION_EMPTY,
            };
        }
    }

    /* SELL rows -- every commodity the station consumes. Always emit the
     * line, even when the player has none of it OR the station's hopper
     * is full -- the row is just passive in those cases (no hotkey, no
     * +N popup). The point is to show market state at a glance. */
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT && row_count < max; c++) {
        if (!station_consumes(st, (commodity_t)c)) continue;
        float price_base = station_buy_price(st, (commodity_t)c);
        if (price_base <= FLOAT_EPSILON) continue;
        int station_inv = (int)lroundf(st->inventory[c]);
        bool station_full = station_inv >= capacity;
        /* Cargo float is authoritative for what's onboard. The manifest
         * can drift past it (wire desync), so cap held counts by cargo
         * before deciding what rows to surface. */
        int cargo_units = (int)floorf(ship_cargo_amount(ship, (commodity_t)c) + 0.0001f);
        if (cargo_units <= 0) continue;
        int manifest_total_for_c = 0;
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
            manifest_total_for_c += ship_manifest_count_cg(ship, (commodity_t)c, (mining_grade_t)gi);
        bool emitted_any = false;
        for (int gi = 0; gi < MINING_GRADE_COUNT && row_count < max; gi++) {
            int manifest_g = ship_manifest_count_cg(ship, (commodity_t)c, (mining_grade_t)gi);
            if (manifest_g <= 0) continue;
            int held = (manifest_total_for_c > 0)
                ? (int)((float)manifest_g * (float)cargo_units / (float)manifest_total_for_c + 0.5f)
                : cargo_units;
            if (held <= 0) continue;
            if (held > cargo_units) held = cargo_units;
            emitted_any = true;
            int price = (int)lroundf(price_base
                    * mining_payout_multiplier((mining_grade_t)gi));
            uint8_t blk = TRADE_BLOCK_NONE;
            if (station_full) blk = TRADE_BLOCK_STATION_FULL;
            out[row_count++] = (trade_row_t){
                .kind = 1, .commodity = (commodity_t)c, .grade = (mining_grade_t)gi,
                .stock = held, .unit_price = price,
                .actionable = (blk == TRADE_BLOCK_NONE), .is_float_fallback = false,
                .station_stock = station_inv, .station_capacity = capacity,
                .held = held, .block_reason = blk,
            };
        }
        /* Cargo says we have units but manifest empty -- fall back to a
         * common-grade row so the player can still sell. */
        if (!emitted_any && row_count < max) {
            int price = (int)lroundf(price_base);
            uint8_t blk = station_full ? TRADE_BLOCK_STATION_FULL : TRADE_BLOCK_NONE;
            out[row_count++] = (trade_row_t){
                .kind = 1, .commodity = (commodity_t)c, .grade = MINING_GRADE_COMMON,
                .stock = cargo_units, .unit_price = price,
                .actionable = (blk == TRADE_BLOCK_NONE), .is_float_fallback = true,
                .station_stock = station_inv, .station_capacity = capacity,
                .held = cargo_units, .block_reason = blk,
            };
        }
    }
    return row_count;
}

static void draw_trade_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    float row_h = compact ? 13.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;
    const uint8_t COL_GAIN[3]  = { 130, 230, 150 };  /* + sell: green */
    const uint8_t COL_COST[3]  = { 230, 110, 110 };  /* - buy:  red   */
    const uint8_t COL_DIM[3]   = { PAL_AFFORD_INACTIVE };
    const uint8_t COL_FADED[3] = { PAL_TEXT_FADED };
    const uint8_t COL_TEXT[3]  = { PAL_TEXT_SECONDARY };

    my += draw_section_header(cx, my, inner_right, "TRADE", HDR_TRADE);

    /* Supply strip — six commodity slots showing this station's chain
     * STATUS, not raw counts. The picker rows below carry the actual
     * stock + price; this row is a flow badge.
     *
     * Three nested stripes per cell — empty/=/==/=== — each higher
     * level implies the lower:
     *   ===  on the shelf  (manifest > 0 && sell_price > 0)
     *   ==-  in flow       (inventory or any module buffer carrying c)
     *   =--  in the system (station has the producing module)
     *   ---  not produced here
     *
     * Color = commodity hue, brightness rises with the level so a
     * station at a glance reads as "alive ferrite line" vs "starved
     * laser line". */
    {
        struct { commodity_t c; module_type_t producer; const char *code; } slots[6] = {
            { COMMODITY_FERRITE_INGOT,  MODULE_FURNACE,      "FE" },
            { COMMODITY_CUPRITE_INGOT,  MODULE_FURNACE_CU,   "CU" },
            { COMMODITY_CRYSTAL_INGOT,  MODULE_FURNACE_CR,   "CR" },
            { COMMODITY_FRAME,          MODULE_FRAME_PRESS,  "FM" },
            { COMMODITY_LASER_MODULE,   MODULE_LASER_FAB,    "LM" },
            { COMMODITY_TRACTOR_MODULE, MODULE_TRACTOR_FAB,  "TM" },
        };
        const float cell_w = 8.0f;
        const float slot_w = cell_w * 7.0f;
        for (int i = 0; i < 6; i++) {
            float sx = cx + (float)i * slot_w;
            commodity_t c = slots[i].c;

            /* Level 1: producer module present? */
            bool in_system = station_produces(st, c);

            /* Level 2: any concrete supply at this station — float
             * inventory OR module buffers carrying this commodity.
             * Module buffers are tagged by the recipe of the host
             * module: input buffer carries the recipe's input;
             * output buffer carries the recipe's output. STORAGE
             * modules don't have a fixed commodity, so we count any
             * non-empty storage output as "in flow" for any
             * commodity the station produces (best the data lets us
             * do without per-tick commodity tags). */
            bool in_flow = (st->inventory[c] > 0.01f);
            if (!in_flow) {
                for (int m = 0; m < st->module_count; m++) {
                    module_type_t mt = st->modules[m].type;
                    if (st->modules[m].scaffold) continue;
                    if (module_schema_input(mt) == c && st->module_input[m] > 0.01f) {
                        in_flow = true; break;
                    }
                    if (module_schema_output(mt) == c && st->module_output[m] > 0.01f) {
                        in_flow = true; break;
                    }
                }
            }

            /* Level 3: stock the player can buy now (same condition
             * the BUY rows in the picker use). */
            int manifest_stock = 0;
            for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
                manifest_stock += station_manifest_count_cg(st, c, (mining_grade_t)gi);
            bool sellable = in_system && (manifest_stock > 0)
                          && (station_sell_price(st, c) > FLOAT_EPSILON);

            int level = 0;
            if (in_system) level = 1;
            if (in_system && in_flow) level = 2;
            if (sellable) level = 3;

            uint8_t r, g, b;
            if (level == 0) {
                r = 90; g = 90; b = 90;
            } else {
                commodity_color_u8(c, &r, &g, &b);
                /* Three brightness tiers: 1/3 / 2/3 / full. */
                int num = level;
                r = (uint8_t)((int)r * num / 3);
                g = (uint8_t)((int)g * num / 3);
                b = (uint8_t)((int)b * num / 3);
            }
            sdtx_color3b(r, g, b);
            sdtx_pos(ui_text_pos(sx), ui_text_pos(my));

            /* Density glyph per level — one character that grows in
             * weight as the chain ascends from idle to sellable.
             * Level 3 uses oric font byte 0xA0 (full block, verified
             * 8×0xFF in vendor/sokol/sokol_debugtext.h). */
            const char *glyph =
                (level == 3) ? "\xA0" :
                (level == 2) ? "=" :
                (level == 1) ? "-" : " ";
            char cell[8];
            snprintf(cell, sizeof(cell), "%s %s", slots[i].code, glyph);
            sdtx_puts(cell);
        }
        my += row_h;
    }

    /* Single source of truth for the row list (shared with input.c so
     * a [1] keypress always hits the same row drawn here). */
    trade_row_t rows[TRADE_MAX_ROWS];
    int row_count = build_trade_rows(st, ship, rows, TRADE_MAX_ROWS);

    /* Pagination — wrap the current page so [F] at the last page
     * returns to page 0 cleanly. TRADE_ROWS_PER_PAGE is small so page
     * counts stay single-digit. */
    int total_pages = (row_count + TRADE_ROWS_PER_PAGE - 1) / TRADE_ROWS_PER_PAGE;
    if (total_pages < 1) total_pages = 1;
    int page = (int)g.trade_page;
    if (page >= total_pages) { page = 0; g.trade_page = 0; }
    int first = page * TRADE_ROWS_PER_PAGE;
    int last  = first + TRADE_ROWS_PER_PAGE;
    if (last > row_count) last = row_count;

    /* Page indicator (only when there's actually more than one page). */
    if (total_pages > 1) {
        char pg[32];
        snprintf(pg, sizeof(pg), "page %d/%d   [F] next",
                 page + 1, total_pages);
        const uint8_t COL_ACTIVE[3] = { 130, 210, 255 };
        draw_row_lr(cx, my, inner_right, COL_ACTIVE, "TRADE", COL_FADED, pg);
        my += row_h;
    }

    if (row_count == 0) {
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "Nothing on offer and nothing to deliver.", NULL, NULL);
        return;
    }

    /* Hotkey numbering walks the page in order but skips passive rows
     * so [1]..[5] always address something the player can actually do. */
    int next_hotkey = 1;
    for (int ri = first; ri < last; ri++) {
        const trade_row_t *r = &rows[ri];
        char key_buf[8];
        if (r->actionable) {
            snprintf(key_buf, sizeof(key_buf), "[%d]", next_hotkey++);
        } else {
            snprintf(key_buf, sizeof(key_buf), "   ");
        }

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

        const char *verb = (r->kind == 0) ? "buy " : "sell";

        /* Status column on the left of the right-aligned price:
         * BUY:  station X/MAX
         * SELL: station X/MAX  (Y held)
         * Passive rows shorten/replace the price column with a reason. */
        char status_buf[40];
        if (r->kind == 0) {
            snprintf(status_buf, sizeof(status_buf), "%d/%d",
                     r->station_stock, r->station_capacity);
        } else {
            snprintf(status_buf, sizeof(status_buf), "%d/%d  (%d held)",
                     r->station_stock, r->station_capacity, r->held);
        }

        char total_buf[32];
        if (r->actionable) {
            if (r->kind == 0) snprintf(total_buf, sizeof(total_buf), "-%d cr", r->unit_price);
            else              snprintf(total_buf, sizeof(total_buf), "+%d cr", r->unit_price);
        } else {
            const char *why = "";
            switch (r->block_reason) {
            case TRADE_BLOCK_STATION_FULL:  why = "(full)";       break;
            case TRADE_BLOCK_STATION_EMPTY: why = "(empty)";      break;
            case TRADE_BLOCK_HOLD_FULL:     why = "(hold full)";  break;
            case TRADE_BLOCK_NO_FUNDS:      why = "(no funds)";   break;
            default:                        why = "";             break;
            }
            snprintf(total_buf, sizeof(total_buf), "%s", why);
        }

        if (compact) {
            cell_t top[] = {
                {  0, key_buf,                            row_rgb },
                {  4, verb,                               row_rgb },
                { 10, commodity_short_name(r->commodity), info_rgb },
                { 26, grade_label,                        grade_rgb_ptr },
            };
            draw_row_cells(cx, my, top, 4);
            my += row_h;
            draw_row_lr(cx + 32.0f, my, inner_right,
                        info_rgb, status_buf, row_rgb, total_buf);
            my += row_h;
        } else {
            cell_t row[] = {
                {  0, key_buf,                            row_rgb },
                {  4, verb,                               row_rgb },
                { 10, commodity_short_name(r->commodity), info_rgb },
                { 28, grade_label,                        grade_rgb_ptr },
                { 35, status_buf,                         info_rgb },
            };
            draw_row_cells(cx, my, row, 5);
            draw_row_lr(cx, my, inner_right, NULL, NULL, row_rgb, total_buf);
            my += row_h;
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
    float row_h = compact ? 13.0f : 15.0f;
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
        int held = (int)lroundf(ship_cargo_amount(ship, COMMODITY_FRAME));
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
                 (int)lroundf(ship_total_cargo(ship)),
                 (int)lroundf(ship_cargo_capacity(ship)));
        draw_row_lr(cx, my, inner_right, COL_TEXT, "cargo", COL_TEXT, right_buf);
        /* Grade-tinted cargo fill bar -- sits inside the cargo row, just
         * below the text baseline so it visually belongs to that row.
         * Segments are sized by manifest unit volume and colored per
         * grade; common-grade swallows any cargo[] float not represented
         * by a manifest unit (e.g. fractional leftovers). */
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
                float manifest_vol = 0.0f;
                for (uint16_t u = 0; u < ship->manifest.count; u++) {
                    const cargo_unit_t *cu = &ship->manifest.units[u];
                    float vol = commodity_volume((commodity_t)cu->commodity);
                    int gi = cu->grade;
                    if (gi < 0 || gi >= MINING_GRADE_COUNT) gi = MINING_GRADE_COMMON;
                    vol_by_grade[gi] += vol;
                    manifest_vol     += vol;
                }
                float total_vol = ship_total_cargo(ship);
                float remainder = total_vol - manifest_vol;
                if (remainder > 0.001f) vol_by_grade[MINING_GRADE_COMMON] += remainder;

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
        draw_row_lr(cx, my, inner_right, COL_TEXT, "modules", COL_TEXT, right_buf);
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
        const uint8_t *left_rgb = COL_AMBER;
        char right_buf[64];
        int kits_avail = ui->ship_kits + ui->station_kits;
        int kits_needed = ui->hull_max - ui->hull_now;
        if (kits_needed < 0) kits_needed = 0;
        if (ui->hull_now >= ui->hull_max) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "kits [ %d / 0 ]",
                     kits_avail);
        } else if (kits_avail <= 0) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "no kits available");
        } else if (ui->can_repair && ui->repair_cost > 0) {
            snprintf(right_buf, sizeof(right_buf), "kits [ %d / %d ]  -%d cr",
                     kits_avail, kits_needed, ui->repair_cost);
        } else if (ui->can_repair) {
            snprintf(right_buf, sizeof(right_buf), "kits [ %d / %d ]",
                     kits_avail, kits_needed);
        } else if (ui->repair_cost > 0) {
            /* Affordability blocks; module supply is fine. */
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "kits [ %d / %d ]  need %d cr",
                     kits_avail, kits_needed, ui->repair_cost);
        } else {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "unavailable here");
        }
        draw_row_lr(cx, my, inner_right, left_rgb, "[R] repair hull",
                    (left_rgb == COL_AMBER) ? COL_TEXT : COL_FADED, right_buf);
        my += row_h;
    }

    /* [M] tune laser, [H] expand hold, [T] tune tractor — same grammar.
     * Real cost is the modules themselves (frames / lasers / tractors
     * pulled from cargo). If cargo is short, the dock fills the gap
     * from its own inventory at retail. Any dock can install — module
     * supply is the only gate. */
    struct { const char *left; const char *unit_singular; const char *unit_plural;
             int needed, in_cargo, at_station, credit; bool can; bool maxed; } refit[3] = {
        { "[M] tune laser",   "laser module",   "laser modules",
          ui->mining_units_needed, ui->mining_units_in_cargo,
          ui->mining_units_at_station, ui->mining_credit_cost,
          ui->can_upgrade_mining,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING) },
        { "[H] expand hold",  "frame", "frames",
          ui->hold_units_needed, ui->hold_units_in_cargo,
          ui->hold_units_at_station, ui->hold_credit_cost,
          ui->can_upgrade_hold,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_HOLD) },
        { "[T] tune tractor", "tractor module", "tractor modules",
          ui->tractor_units_needed, ui->tractor_units_in_cargo,
          ui->tractor_units_at_station, ui->tractor_credit_cost,
          ui->can_upgrade_tractor,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_TRACTOR) },
    };
    for (int i = 0; i < 3; i++) {
        const uint8_t *left_rgb = COL_NAV;
        char right_buf[80];
        int avail  = refit[i].in_cargo + refit[i].at_station;
        int needed = refit[i].needed;
        const char *plural = refit[i].unit_plural;
        if (refit[i].maxed) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "maxed");
        } else if (avail <= 0) {
            /* Mirrors the [R] repair-hull row: nothing in cargo or at
             * the dock, no way to install. */
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "no %s available", plural);
        } else {
            if (!refit[i].can) left_rgb = COL_DIM;
            /* Always: "<plural> [ have / need ]". If the row is
             * actionable AND the dock is filling some of the gap from
             * its inventory, append the credit cost the player will pay
             * at retail. */
            if (refit[i].can && refit[i].credit > 0) {
                snprintf(right_buf, sizeof(right_buf), "%s [ %d / %d ]  -%d cr",
                         plural, avail, needed,
                         refit[i].credit);
            } else {
                snprintf(right_buf, sizeof(right_buf), "%s [ %d / %d ]",
                         plural, avail, needed);
            }
        }
        draw_row_lr(cx, my, inner_right, left_rgb, refit[i].left,
                    (left_rgb == COL_NAV) ? COL_TEXT : COL_FADED, right_buf);
        my += row_h;
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
    float row_h = compact ? 13.0f : 15.0f;
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

        /* Job column doubles as the state verb — the action the player
         * needs to take next. FRACTURE contracts always read "fracture".
         * TRACTOR contracts pivot based on hold + world:
         *   - held enough                          → "deliver"
         *   - ingot/frame (never a raw ore tow)    → "deliver"
         *   - an S-tier fragment of this commodity
         *     exists in the world                  → "tractor"
         *   - no fragments yet                     → "fracture" */
        const char *job_txt;
        if (ct->action == CONTRACT_FRACTURE) {
            job_txt = "fracture";
        } else if (slot_fulfillable[s]) {
            job_txt = "deliver";
        } else if (ct->commodity >= COMMODITY_RAW_ORE_COUNT) {
            /* Ingots / frames / fab goods — always the delivery verb. */
            job_txt = "deliver";
        } else {
            /* Raw ore, not yet held: tractor if a matching S-tier
             * fragment exists anywhere, else fracture. */
            bool any_frag = false;
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                const asteroid_t *a = &g.world.asteroids[i];
                if (!a->active) continue;
                if (a->tier != ASTEROID_TIER_S) continue;
                if (a->commodity != ct->commodity) continue;
                any_frag = true; break;
            }
            job_txt = any_frag ? "tractor" : "fracture";
        }

        if (ct->action == CONTRACT_FRACTURE) {
            snprintf(cargo_buf, sizeof(cargo_buf), "asteroid field");
        } else {
            int qty = slot_fulfillable[s] ? slot_held[s]
                                          : (int)lroundf(ct->quantity_needed);
            /* Drop the grade word — color encodes rarity for the whole
             * row (see the grade-tint override below). Keeps the cargo
             * cell short so payout doesn't collide with it. */
            snprintf(cargo_buf, sizeof(cargo_buf), "%s x%d",
                     commodity_short_name(ct->commodity), qty);
        }

        const station_t *dest = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;
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
        } else {
            cell_t row[] = {
                {  0, key_buf,   row_rgb },
                {  4, job_txt,   row_rgb },
                { 14, cargo_buf, cargo_rgb },
                { 33, pay_buf,   row_rgb },
            };
            draw_row_cells(cx, my, row, 4);
            my += row_h;
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
        float row_h = compact ? 13.0f : 15.0f;
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
        sdtx_printf("[%d] %-14s %d %s + %d %s",
            shown + 1, module_type_name(kit),
            fee, ui_station_currency(ui->station),
            mat, mat_name);
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
            float station_have = ui->station->inventory[mat_type];
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

    /* Tab strip, LEFT-aligned on the first content line.
     *   DOCK  — ship bay (repair / refit / current ship state)
     *   TRADE — market (buy / sell cargo)
     *   WORK  — contracts (jobs / routing)
     *   YARD  — fabrication (kits + construction queue)
     * Active tab: station-role tint + a short underline latch. Inactive:
     * muted. The nav legend sits flush against the panel right edge. */
    {
        const char *labels[STATION_VIEW_COUNT] = { "DOCK", "TRADE", "WORK", "YARD" };
        const float cell_w = 8.0f;
        float ty = content_top + 2.0f;
        float tx = cx;
        /* Record active tab geometry so we can draw the latch after the
         * text pass (so the quad doesn't clobber glyphs). */
        float active_x0 = 0.0f, active_x1 = 0.0f;
        bool active_seen = false;
        for (int i = 0; i < (int)STATION_VIEW_COUNT; i++) {
            bool active = ((int)g.station_view == i);
            char cell[20];
            snprintf(cell, sizeof(cell), active ? "[%s]" : " %s ", labels[i]);
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
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_pos(ui_text_pos(panel_x + panel_w - 20.0f - hint_w),
                 ui_text_pos(ty));
        sdtx_puts(hint);

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

    switch (g.station_view) {
    case STATION_VIEW_DOCK:
        draw_verbs_view(ui, cx, cy, inner_w, compact);
        break;
    case STATION_VIEW_TRADE:
        draw_trade_view(ui, cx, cy, inner_w, compact);
        break;
    case STATION_VIEW_WORK:
        draw_jobs_view(ui, cx, cy, inner_w, compact);
        break;
    case STATION_VIEW_YARD:
        draw_yard_view(ui, cx, cy, inner_w, compact);
        break;
    case STATION_VIEW_COUNT:
        break; /* unreachable — enum terminator */
    }

    (void)panel_h;
}
