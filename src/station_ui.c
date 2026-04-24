/*
 * station_ui.c -- Station lookup helpers, formatting, and the docked station
 * services text renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "palette.h"
#include "mining_client.h"
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
/* Station service / upgrade helpers                                   */
/* ------------------------------------------------------------------ */

bool station_has_service(uint32_t service) {
    const station_t* station = current_station_ptr();
    return (station != NULL) && ((station->services & service) != 0);
}

uint32_t station_upgrade_service(ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return STATION_SERVICE_UPGRADE_LASER;
        case SHIP_UPGRADE_HOLD:
            return STATION_SERVICE_UPGRADE_HOLD;
        case SHIP_UPGRADE_TRACTOR:
            return STATION_SERVICE_UPGRADE_TRACTOR;
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
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
    ui->mining_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_MINING);
    ui->hold_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_HOLD);
    ui->tractor_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_TRACTOR);
    ui->can_repair = station_has_service(STATION_SERVICE_REPAIR) && (repair > 0.0f) && (player_current_balance() + FLOAT_EPSILON >= repair);
    float bal = player_current_balance();
    ui->can_upgrade_mining = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_MINING, STATION_SERVICE_UPGRADE_LASER, ui->mining_cost, bal);
    ui->can_upgrade_hold = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, ui->hold_cost, bal);
    ui->can_upgrade_tractor = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_TRACTOR, STATION_SERVICE_UPGRADE_TRACTOR, ui->tractor_cost, bal);
}

/* ------------------------------------------------------------------ */
/* Currency + ingot stock helpers                                      */
/* ------------------------------------------------------------------ */

/* Station-local currency label, falls back to "cr". */
static const char *ui_station_currency(const station_t *st) {
    if (!st) return "cr";
    return (st->currency_name[0]) ? st->currency_name : "cr";
}

#if 0
/* Retained for reference: legacy service-line builders used by the
 * deleted MARKET / STATUS tabs. The verb-list view computes its rows
 * inline from station + ship state and doesn't need this builder. */
int build_station_service_lines(const station_ui_state_t* ui, station_service_line_t lines[3]) {
    if (!ui->station) {
        return 0;
    }

    memset(lines, 0, sizeof(station_service_line_t) * 3);
    int count = 0;

    /* [1] Deliver — for stations that accept player cargo (ingots/frames) */
    commodity_t buy = station_primary_buy(ui->station);
    if ((int)buy >= 0) {
        int held = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship, buy));
        lines[count].action = "[1] Deliver";
        if (held > 0) {
            snprintf(lines[count].state, sizeof(lines[count].state), "%s x%d", commodity_short_name(buy), held);
            lines[count].r = 114; lines[count].g0 = 255; lines[count].b = 192;
        } else {
            snprintf(lines[count].state, sizeof(lines[count].state), "no %s", commodity_short_name(buy));
            lines[count].r = 169; lines[count].g0 = 179; lines[count].b = 204;
        }
        count++;
    }

    lines[count].action = "[2] Repair hull";
    if (ui->repair_cost > 0) {
        snprintf(lines[count].state, sizeof(lines[count].state), "%d %s", ui->repair_cost, ui_station_currency(ui->station));
        lines[count].r = 255;
        lines[count].g0 = 221;
        lines[count].b = 119;
    } else {
        snprintf(lines[count].state, sizeof(lines[count].state), "nominal");
        lines[count].r = 169;
        lines[count].g0 = 179;
        lines[count].b = 204;
    }
    count++;

    if (station_has_module(ui->station, MODULE_FRAME_PRESS) && count < 3) {
        lines[count].action = "[4] Hold racks";
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_HOLD)) {
            snprintf(lines[count].state, sizeof(lines[count].state), "maxed");
            lines[count].r = 169; lines[count].g0 = 179; lines[count].b = 204;
        } else {
            snprintf(lines[count].state, sizeof(lines[count].state), "%d %s", ui->hold_cost, ui_station_currency(ui->station));
            lines[count].r = ui->can_upgrade_hold ? 203 : 169;
            lines[count].g0 = ui->can_upgrade_hold ? 220 : 179;
            lines[count].b = ui->can_upgrade_hold ? 248 : 204;
        }
        count++;
    }

    if (station_has_module(ui->station, MODULE_LASER_FAB) && count < 3) {
        lines[count].action = "[3] Laser array";
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_MINING)) {
            snprintf(lines[count].state, sizeof(lines[count].state), "maxed");
            lines[count].r = 169;
            lines[count].g0 = 179;
            lines[count].b = 204;
        } else {
            snprintf(lines[count].state, sizeof(lines[count].state), "%d %s", ui->mining_cost, ui_station_currency(ui->station));
            lines[count].r = ui->can_upgrade_mining ? 203 : 169;
            lines[count].g0 = ui->can_upgrade_mining ? 220 : 179;
            lines[count].b = ui->can_upgrade_mining ? 248 : 204;
        }
        count++;
    }

    if (station_has_module(ui->station, MODULE_TRACTOR_FAB) && count < 3) {
        lines[count].action = "[5] Tractor coil";
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_TRACTOR)) {
            snprintf(lines[count].state, sizeof(lines[count].state), "maxed");
            lines[count].r = 169;
            lines[count].g0 = 179;
            lines[count].b = 204;
        } else {
            snprintf(lines[count].state, sizeof(lines[count].state), "%d %s", ui->tractor_cost, ui_station_currency(ui->station));
            lines[count].r = ui->can_upgrade_tractor ? 203 : 169;
            lines[count].g0 = ui->can_upgrade_tractor ? 220 : 179;
            lines[count].b = ui->can_upgrade_tractor ? 248 : 204;
        }
        count++;
    }
    return count;
}

void draw_station_service_text_line(float x, float y, const station_service_line_t* line, bool compact) {
    sdtx_pos(ui_text_pos(x), ui_text_pos(y));
    sdtx_color3b(line->r, line->g0, line->b); /* line color override */
    if (compact) {
        sdtx_printf("%-16s %s", line->action, line->state);
    } else {
        sdtx_printf("%-26s %s", line->action, line->state);
    }
}
#endif


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
        char right2[64];
        int balance = (int)lroundf(player_current_balance());
        float sig = signal_strength_at(&g.world, st->pos);
        snprintf(right2, sizeof(right2), "ledger %d %s   sig %.2f",
                 balance, ui_station_currency(st), sig);
        float right2_w = (float)strlen(right2) * cell_w;
        float gap = 16.0f;
        bool fits = (left_x + role_w + gap + right2_w)
                  <= (panel_x + panel_w - right_margin);
        /* If the full string wouldn't fit, try just "ledger N cur" (drop
         * sig); if that still doesn't fit, drop the whole right side. */
        if (!fits) {
            snprintf(right2, sizeof(right2), "ledger %d %s",
                     balance, ui_station_currency(st));
            right2_w = (float)strlen(right2) * cell_w;
            fits = (left_x + role_w + gap + right2_w)
                 <= (panel_x + panel_w - right_margin);
        }
        if (fits) {
            sdtx_pos(ui_text_pos(panel_x + panel_w - right_margin - right2_w),
                     ui_text_pos(panel_y + HEADER_L2));
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_puts(right2);
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

static bool station_manifest_has_commodity(const station_t *st, commodity_t c) {
    if (!st) return false;
    int s = station_index_of(st);
    if (s < 0 || s >= MAX_STATIONS) return false;
    if ((int)c < 0 || (int)c >= COMMODITY_COUNT) return false;
    for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
        if (g.station_manifest_summary[s][c][gi] > 0) return true;
    return false;
}

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

static bool ship_manifest_has_commodity(const ship_t *ship, commodity_t c) {
    if (!ship || !ship->manifest.units) return false;
    for (uint16_t i = 0; i < ship->manifest.count; i++)
        if (ship->manifest.units[i].commodity == (uint8_t)c) return true;
    return false;
}

/* TRADE view — market.
 *
 * Wide layout (single-row table):
 *   key(4) side(6) item(18) grade(7) qty/stock(12)  total(right-aligned)
 * Compact layout (two-line rows):
 *   [F] buy  FE Ingot  common
 *       stock 8                              -41 prospect vouchers
 * The column-header row is dropped in compact since two-line rows are
 * self-describing.
 *
 * Grade split: BUY rows read the station manifest grouped by grade
 * (one row per non-zero {commodity, grade}). When the manifest has no
 * units for this commodity (legacy saves, or multiplayer where the
 * per-station manifest summary isn't wired over the net yet), falls
 * back to the legacy float `inventory[]` as a single COMMON row.
 *
 * SELL rows stay single-row for now — ship.cargo[] is still a grade-less
 * float. Grouping the player's hold by grade needs the ship-manifest
 * transfer path, which is the next slice after server-side manifest-unit
 * buy/sell and a multiplayer net summary. */
static void draw_trade_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    float row_h = compact ? 13.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;

    const uint8_t COL_HDR[3]   = { PAL_TEXT_FADED };
    const uint8_t COL_BUY[3]   = { PAL_CONTRACT_AFFORD };
    const uint8_t COL_SELL[3]  = { PAL_CONTRACT_READY };
    const uint8_t COL_DIM[3]   = { PAL_AFFORD_INACTIVE };
    const uint8_t COL_FADED[3] = { PAL_TEXT_FADED };
    const uint8_t COL_TEXT[3]  = { PAL_TEXT_SECONDARY };

    my += draw_section_header(cx, my, inner_right, "MARKET", HDR_TRADE);

    /* Header row is only useful when everything fits on one line. */
    if (!compact) {
        cell_t hdr[] = {
            {  0, "key",  COL_HDR },
            {  4, "side", COL_HDR },
            { 10, "item", COL_HDR },
            { 28, "grade", COL_HDR },
            { 35, "qty/stock", COL_HDR },
        };
        draw_row_cells(cx, my, hdr, 5);
        draw_row_lr(cx, my, inner_right, NULL, NULL, COL_HDR, "total");
        my += row_h;
    }

    /* Single grade tint computed once; matches MINING_GRADE_COMMON today,
     * wire in per-row grade later. */
    uint8_t gr, gg, gb;
    mining_grade_rgb(MINING_GRADE_COMMON, &gr, &gg, &gb);
    uint8_t grade_rgb[3] = { gr, gg, gb };

    commodity_t sell_c = station_primary_sell(st);
    commodity_t buy_c  = station_primary_buy(st);

    bool any_row = false;

    /* BUY rows — station sells sell_c to the player. When the manifest
     * has entries for this commodity, render one row per non-zero grade
     * (rare / RATi / commissioned get their own rows with the canonical
     * grade color). When the manifest has no entries (legacy float-only
     * inventory, or multiplayer before the net summary lands), fall back
     * to a single COMMON row derived from the legacy float stock.
     *
     * Note: the [F] buy action still operates on the legacy float path
     * (see server/game_sim.c). Until the server-side manifest-unit
     * transfer slice lands, pressing [F] will always draw from whatever
     * unit floor(inventory) happens to hit first. Showing per-grade
     * stock here lets players see what exists on the hopper — the buy
     * itself matching the selected grade is the next slice. */
    if ((int)sell_c >= 0) {
        float price_f = station_sell_price(st, sell_c);
        int   price   = (int)lroundf(price_f);
        float space   = ship_cargo_capacity(ship) - ship_total_cargo(ship);
        float credits = player_current_balance();
        int   afford  = (price_f > FLOAT_EPSILON) ? (int)floorf(credits / price_f) : 0;

        bool rendered_any_grade = false;
        if (station_manifest_has_commodity(st, sell_c)) {
            for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
                int stock = station_manifest_count_cg(st, sell_c, (mining_grade_t)gi);
                if (stock <= 0) continue;
                int can = (int)fminf(fminf((float)stock, space), (float)afford);
                bool actionable = (can > 0);

                uint8_t ggr, ggg, ggb;
                mining_grade_rgb((mining_grade_t)gi, &ggr, &ggg, &ggb);
                uint8_t gr_rgb[3] = { ggr, ggg, ggb };

                const uint8_t *row_rgb = actionable ? COL_BUY : COL_DIM;
                const uint8_t *info_rgb = actionable ? COL_TEXT : COL_FADED;
                char qty_buf[24], total_buf[32];
                if (actionable) {
                    snprintf(qty_buf, sizeof(qty_buf),
                             compact ? "stock %d" : "%d / %d",
                             compact ? stock : can, stock);
                    /* Multiplier-adjusted price: a rare ingot is worth
                     * more than a common one at the same station. */
                    int p2 = (int)lroundf(price_f * mining_payout_multiplier((mining_grade_t)gi));
                    snprintf(total_buf, sizeof(total_buf), "-%d %s",
                             can * p2, ui_station_currency(st));
                } else if (space < 0.5f) {
                    snprintf(qty_buf, sizeof(qty_buf), "hold full");
                    total_buf[0] = '\0';
                } else {
                    snprintf(qty_buf, sizeof(qty_buf),
                             compact ? "stock %d" : "0 / %d", stock);
                    int p2 = (int)lroundf(price_f * mining_payout_multiplier((mining_grade_t)gi));
                    snprintf(total_buf, sizeof(total_buf),
                             "need %d %s", p2, ui_station_currency(st));
                }

                if (compact) {
                    cell_t top[] = {
                        {  0, "[F]",                        row_rgb },
                        {  4, "buy",                        row_rgb },
                        { 10, commodity_short_name(sell_c), info_rgb },
                        { 26, mining_grade_label((mining_grade_t)gi), gr_rgb },
                    };
                    draw_row_cells(cx, my, top, 4);
                    my += row_h;
                    draw_row_lr(cx + 32.0f, my, inner_right,
                                info_rgb, qty_buf,
                                info_rgb, total_buf[0] ? total_buf : NULL);
                    my += row_h;
                } else {
                    cell_t row[] = {
                        {  0, "[F]",                        row_rgb },
                        {  4, "buy",                        row_rgb },
                        { 10, commodity_short_name(sell_c), info_rgb },
                        { 28, mining_grade_label((mining_grade_t)gi), gr_rgb },
                        { 35, qty_buf,                      info_rgb },
                    };
                    draw_row_cells(cx, my, row, 5);
                    if (total_buf[0])
                        draw_row_lr(cx, my, inner_right, NULL, NULL, info_rgb, total_buf);
                    my += row_h;
                }
                rendered_any_grade = true;
            }
        }

        if (!rendered_any_grade) {
            /* Legacy fallback: no manifest entries. Show the float total
             * as a single COMMON row. */
            float availf = station_inventory_amount(st, sell_c);
            int   avail  = (int)lroundf(availf);
            int   can    = (int)fminf(fminf(availf, space), (float)afford);
            bool  actionable = (can > 0);
            const uint8_t *row_rgb = actionable ? COL_BUY : COL_DIM;
            const uint8_t *info_rgb = actionable ? COL_TEXT : COL_FADED;
            char qty_buf[24], total_buf[32];
            if (actionable) {
                snprintf(qty_buf, sizeof(qty_buf),
                         compact ? "stock %d" : "%d / %d",
                         compact ? avail : can, avail);
                snprintf(total_buf, sizeof(total_buf), "-%d %s",
                         can * price, ui_station_currency(st));
            } else if (availf < 0.5f) {
                snprintf(qty_buf, sizeof(qty_buf), "out of stock");
                total_buf[0] = '\0';
            } else if (space < 0.5f) {
                snprintf(qty_buf, sizeof(qty_buf), "hold full");
                total_buf[0] = '\0';
            } else {
                snprintf(qty_buf, sizeof(qty_buf),
                         compact ? "stock %d" : "0 / %d", avail);
                snprintf(total_buf, sizeof(total_buf),
                         "need %d %s", price, ui_station_currency(st));
            }
            if (!(availf < 0.5f && !actionable)) {
                if (compact) {
                    cell_t top[] = {
                        {  0, "[F]",                        row_rgb },
                        {  4, "buy",                        row_rgb },
                        { 10, commodity_short_name(sell_c), info_rgb },
                        { 26, mining_grade_label(MINING_GRADE_COMMON), grade_rgb },
                    };
                    draw_row_cells(cx, my, top, 4);
                    my += row_h;
                    draw_row_lr(cx + 32.0f, my, inner_right,
                                info_rgb, qty_buf,
                                info_rgb, total_buf[0] ? total_buf : NULL);
                    my += row_h;
                } else {
                    cell_t row[] = {
                        {  0, "[F]",                        row_rgb },
                        {  4, "buy",                        row_rgb },
                        { 10, commodity_short_name(sell_c), info_rgb },
                        { 28, mining_grade_label(MINING_GRADE_COMMON), grade_rgb },
                        { 35, qty_buf,                      info_rgb },
                    };
                    draw_row_cells(cx, my, row, 5);
                    if (total_buf[0])
                        draw_row_lr(cx, my, inner_right, NULL, NULL, info_rgb, total_buf);
                    my += row_h;
                }
                rendered_any_grade = true;
            }
        }
        if (rendered_any_grade) any_row = true;
    }

    /* SELL rows — station buys buy_c from the player's hold.
     * Phase 3: if the ship.manifest carries units for this commodity,
     * render one row per grade held. Otherwise fall back to the legacy
     * single row using the ship.cargo[] float so pre-manifest ingots
     * still sell. The [S] action itself still operates on the float via
     * the server's dual-write path (sim side is manifest-first, UI
     * reflects the same grouping). */
    if ((int)buy_c >= 0) {
        float base_price = station_buy_price(st, buy_c);
        bool rendered = false;

        if (ship_manifest_has_commodity(ship, buy_c)) {
            for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
                int held = ship_manifest_count_cg(ship, buy_c, (mining_grade_t)gi);
                if (held <= 0) continue;
                int price = (int)lroundf(
                    base_price * mining_payout_multiplier((mining_grade_t)gi));
                bool actionable = (held > 0);
                const uint8_t *row_rgb = actionable ? COL_SELL : COL_DIM;
                const uint8_t *info_rgb = actionable ? COL_TEXT : COL_FADED;

                uint8_t ggr, ggg, ggb;
                mining_grade_rgb((mining_grade_t)gi, &ggr, &ggg, &ggb);
                uint8_t gr_rgb[3] = { ggr, ggg, ggb };

                char qty_buf[24], total_buf[32];
                snprintf(qty_buf, sizeof(qty_buf), "%d held", held);
                snprintf(total_buf, sizeof(total_buf), "+%d %s",
                         held * price, ui_station_currency(st));

                if (compact) {
                    cell_t top[] = {
                        {  0, "[S]",                       row_rgb },
                        {  4, "sell",                      row_rgb },
                        { 10, commodity_short_name(buy_c), info_rgb },
                        { 26, mining_grade_label((mining_grade_t)gi), gr_rgb },
                    };
                    draw_row_cells(cx, my, top, 4);
                    my += row_h;
                    draw_row_lr(cx + 32.0f, my, inner_right,
                                info_rgb, qty_buf, info_rgb, total_buf);
                    my += row_h;
                } else {
                    cell_t row[] = {
                        {  0, "[S]",                       row_rgb },
                        {  4, "sell",                      row_rgb },
                        { 10, commodity_short_name(buy_c), info_rgb },
                        { 28, mining_grade_label((mining_grade_t)gi), gr_rgb },
                        { 35, qty_buf,                     info_rgb },
                    };
                    draw_row_cells(cx, my, row, 5);
                    draw_row_lr(cx, my, inner_right, NULL, NULL, info_rgb, total_buf);
                    my += row_h;
                }
                rendered = true;
            }
        }

        if (!rendered) {
            /* Legacy fallback: grade-less float. Show a single COMMON row. */
            int held = (int)lroundf(ship_cargo_amount(ship, buy_c));
            int price = (int)lroundf(base_price);
            bool actionable = (held > 0);
            const uint8_t *row_rgb = actionable ? COL_SELL : COL_DIM;
            const uint8_t *info_rgb = actionable ? COL_TEXT : COL_FADED;
            char qty_buf[24], total_buf[32];
            if (actionable) {
                snprintf(qty_buf, sizeof(qty_buf), "%d held", held);
                snprintf(total_buf, sizeof(total_buf), "+%d %s",
                         held * price, ui_station_currency(st));
            } else {
                snprintf(qty_buf, sizeof(qty_buf), "none held");
                snprintf(total_buf, sizeof(total_buf),
                         "+%d %s ea.", price, ui_station_currency(st));
            }
            if (compact) {
                cell_t top[] = {
                    {  0, "[S]",                       row_rgb },
                    {  4, "sell",                      row_rgb },
                    { 10, commodity_short_name(buy_c), info_rgb },
                    { 26, mining_grade_label(MINING_GRADE_COMMON), grade_rgb },
                };
                draw_row_cells(cx, my, top, 4);
                my += row_h;
                draw_row_lr(cx + 32.0f, my, inner_right,
                            info_rgb, qty_buf, info_rgb, total_buf);
                my += row_h;
            } else {
                cell_t row[] = {
                    {  0, "[S]",                       row_rgb },
                    {  4, "sell",                      row_rgb },
                    { 10, commodity_short_name(buy_c), info_rgb },
                    { 28, mining_grade_label(MINING_GRADE_COMMON), grade_rgb },
                    { 35, qty_buf,                     info_rgb },
                };
                draw_row_cells(cx, my, row, 5);
                draw_row_lr(cx, my, inner_right, NULL, NULL, info_rgb, total_buf);
                my += row_h;
            }
        }
        any_row = true;
    }

    if (!any_row) {
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "Nothing on offer and nothing to deliver.", NULL, NULL);
    }
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
        my += row_h;

        snprintf(right_buf, sizeof(right_buf), "LSR %d  HLD %d  TRC %d",
                 ship->mining_level, ship->hold_level, ship->tractor_level);
        draw_row_lr(cx, my, inner_right, COL_TEXT, "modules", COL_TEXT, right_buf);
        my += row_h;
    }
    my += 6.0f;

    /* -------- SERVICES (always visible; rows always show their status) -------- */
    my += draw_section_header(cx, my, inner_right, "SERVICES", HDR_SERVICE);

    /* [R] repair hull */
    {
        const uint8_t *left_rgb = COL_AMBER;
        char right_buf[48];
        if (ui->hull_now >= ui->hull_max) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "hull full");
        } else if (ui->can_repair) {
            snprintf(right_buf, sizeof(right_buf), "-%d %s",
                     ui->repair_cost, ui_station_currency(st));
        } else if (ui->repair_cost > 0) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "need %d %s",
                     ui->repair_cost, ui_station_currency(st));
        } else {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "unavailable here");
        }
        draw_row_lr(cx, my, inner_right, left_rgb, "[R] repair hull",
                    (left_rgb == COL_AMBER) ? COL_TEXT : COL_FADED, right_buf);
        my += row_h;
    }

    /* [M] tune laser, [H] expand hold, [T] tune tractor — same grammar. */
    struct { const char *left; module_type_t gate; int cost; bool can;
             bool maxed; } refit[3] = {
        { "[M] tune laser",   MODULE_LASER_FAB,   ui->mining_cost,  ui->can_upgrade_mining,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING) },
        { "[H] expand hold",  MODULE_FRAME_PRESS, ui->hold_cost,    ui->can_upgrade_hold,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_HOLD) },
        { "[T] tune tractor", MODULE_TRACTOR_FAB, ui->tractor_cost, ui->can_upgrade_tractor,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_TRACTOR) },
    };
    for (int i = 0; i < 3; i++) {
        const uint8_t *left_rgb = COL_NAV;
        char right_buf[48];
        if (refit[i].maxed) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "maxed");
        } else if (!station_has_module(st, refit[i].gate)) {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "unavailable here");
        } else if (refit[i].can) {
            snprintf(right_buf, sizeof(right_buf), "-%d %s",
                     refit[i].cost, ui_station_currency(st));
        } else {
            left_rgb = COL_DIM;
            snprintf(right_buf, sizeof(right_buf), "need %d %s",
                     refit[i].cost, ui_station_currency(st));
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

    /* Column header row. */
    /* Header row only fits on a wide panel; compact uses two-line rows. */
    if (!compact) {
        cell_t hdr[] = {
            {  0, "key",   COL_HDR },
            {  4, "job",   COL_HDR },
            { 14, "cargo", COL_HDR },
            { 33, "state", COL_HDR },
        };
        draw_row_cells(cx, my, hdr, 4);
        draw_row_lr(cx, my, inner_right, NULL, NULL, COL_HDR, "payout");
        my += row_h;
    }

    /* Build slot listing (same logic as before: fulfillable here first,
     * then nearest by distance). */
    int slots[3] = {-1, -1, -1};
    int slot_count = 0;
    bool slot_fulfillable[3] = {false, false, false};
    int slot_held[3] = {0, 0, 0};
    int here_idx = LOCAL_PLAYER.current_station;

    for (int ci = 0; ci < MAX_CONTRACTS && slot_count < 3; ci++) {
        contract_t *ct = &g.world.contracts[ci];
        if (!ct->active) continue;
        if (ct->action != CONTRACT_TRACTOR) continue;
        if (here_idx < 0 || ct->station_index != here_idx) continue;
        float held = LOCAL_PLAYER.ship.cargo[ct->commodity];
        if (held < 0.5f) continue;
        slots[slot_count] = ci;
        slot_fulfillable[slot_count] = true;
        slot_held[slot_count] = (int)lroundf(held);
        slot_count++;
    }
    if (slot_count < 3) {
        int nearest[3] = {-1, -1, -1};
        float nearest_d[3] = {1e18f, 1e18f, 1e18f};
        vec2 here = ui->station->pos;
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->station_index >= MAX_STATIONS) continue;
            if (!station_exists(&g.world.stations[ct->station_index])) continue;
            bool already = false;
            for (int s = 0; s < slot_count; s++)
                if (slots[s] == ci) { already = true; break; }
            if (already) continue;
            vec2 target = (ct->action == CONTRACT_TRACTOR)
                ? g.world.stations[ct->station_index].pos : ct->target_pos;
            float d = v2_dist_sq(here, target);
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
        for (int s = 0; s < 3 && slot_count < 3; s++) {
            if (nearest[s] < 0) continue;
            slots[slot_count] = nearest[s];
            slot_fulfillable[slot_count] = false;
            slot_held[slot_count] = 0;
            slot_count++;
        }
    }

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

        char key_buf[8], cargo_buf[32], state_buf[16], pay_buf[32];
        snprintf(key_buf, sizeof(key_buf), "[%d]%s",
                 s + 1, tracked && !selected ? "*" : "");

        const char *job_txt = (ct->action == CONTRACT_FRACTURE) ? "fracture" : "deliver";

        if (ct->action == CONTRACT_FRACTURE) {
            snprintf(cargo_buf, sizeof(cargo_buf), "asteroid field");
        } else {
            int qty = slot_fulfillable[s] ? slot_held[s]
                                          : (int)lroundf(ct->quantity_needed);
            snprintf(cargo_buf, sizeof(cargo_buf), "%s x%d",
                     commodity_short_name(ct->commodity), qty);
        }

        if (ct->action == CONTRACT_FRACTURE) {
            snprintf(state_buf, sizeof(state_buf), "nearby");
        } else if (slot_fulfillable[s]) {
            snprintf(state_buf, sizeof(state_buf), "ready");
        } else {
            snprintf(state_buf, sizeof(state_buf), "missing");
        }

        const station_t *dest = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;
        const char *pay_cur = ui_station_currency(dest ? dest : ui->station);
        /* Defensive clamp: if the server ever sends a nonsense base_price
         * (NaN/inf/garbage) the naive lroundf cast saturates to INT_MAX
         * and the row displays +2147483647. Show "???" instead so the
         * bug is visible and not mistaken for a real payout. */
        if (!isfinite(cprice) || cprice < 0.0f || cprice > 1.0e7f) {
            snprintf(pay_buf, sizeof(pay_buf), "+??? %s", pay_cur);
        } else {
            snprintf(pay_buf, sizeof(pay_buf), "+%d %s",
                     (int)lroundf(cprice), pay_cur);
        }

        const uint8_t *info_rgb = (row_rgb == COL_DIM) ? COL_FADED : COL_TEXT;
        if (compact) {
            /* Line 1: key / job / cargo */
            cell_t top[] = {
                {  0, key_buf,   row_rgb },
                {  4, job_txt,   row_rgb },
                { 14, cargo_buf, info_rgb },
            };
            draw_row_cells(cx, my, top, 3);
            my += row_h;
            /* Line 2: indented state on left, payout right-aligned */
            draw_row_lr(cx + 32.0f, my, inner_right,
                        row_rgb, state_buf, row_rgb, pay_buf);
            my += row_h;
        } else {
            cell_t row[] = {
                {  0, key_buf,   row_rgb },
                {  4, job_txt,   row_rgb },
                { 14, cargo_buf, info_rgb },
                { 33, state_buf, row_rgb },
            };
            draw_row_cells(cx, my, row, 4);
            draw_row_lr(cx, my, inner_right, NULL, NULL, row_rgb, pay_buf);
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
        sdtx_color3b(can_afford ? PAL_TEXT_SECONDARY : PAL_CANNOT_AFFORD);
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
