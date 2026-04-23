/*
 * station_ui.c -- Station lookup helpers, formatting, and the docked station
 * services text renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "palette.h"
#include "mining_client.h"

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
    const ship_t *ship = &LOCAL_PLAYER.ship;
    float left_x = panel_x + 20.0f;
    float right_margin = 20.0f;
    const float cell_w = 8.0f;

    /* Line 1: station name (left)  ·  ledger here, signal (right) */
    sdtx_color3b(PAL_TEXT_PRIMARY);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + 12.0f));
    sdtx_puts(st->name);

    if (panel_w >= 360.0f) {
        char right1[64];
        int balance = (int)lroundf(player_current_balance());
        float sig = signal_strength_at(&g.world, st->pos);
        snprintf(right1, sizeof(right1), "ledger %d %s   sig %.2f",
                 balance, ui_station_currency(st), sig);
        float right1_w = (float)strlen(right1) * cell_w;
        sdtx_pos(ui_text_pos(panel_x + panel_w - right_margin - right1_w),
                 ui_text_pos(panel_y + 12.0f));
        sdtx_color3b(PAL_TEXT_SECONDARY);
        sdtx_puts(right1);
    }

    /* Line 2: role (left)  ·  hull + cargo (right) */
    sdtx_color3b(PAL_HOLD_CYAN);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + 26.0f));
    sdtx_puts(station_role_hub_label(st));

    if (panel_w >= 360.0f) {
        char right2[64];
        snprintf(right2, sizeof(right2), "hull %d/%d  cargo %d/%d   [E] LAUNCH",
                 (int)lroundf(ship->hull),
                 (int)lroundf(ship_max_hull(ship)),
                 (int)lroundf(ship_total_cargo(ship)),
                 (int)lroundf(ship_cargo_capacity(ship)));
        float right2_w = (float)strlen(right2) * cell_w;
        sdtx_pos(ui_text_pos(panel_x + panel_w - right_margin - right2_w),
                 ui_text_pos(panel_y + 26.0f));
        sdtx_color3b(PAL_STATION_HINT);
        sdtx_puts(right2);
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
            sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + 42.0f));
            sdtx_puts(line);
        }
    }
}

/* ------------------------------------------------------------------ */
/* VERBS view — the new default action surface                         */
/* ------------------------------------------------------------------ */

static void draw_verbs_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    float row_h = compact ? 13.0f : 15.0f;
    float right_x = cx + fminf(inner_w - 36.0f, 360.0f);
    float my = cy;

    if (st->scaffold) {
        /* Special case: docked at a station that's still being built.
         * The "verb" here is delivering frames to advance construction. */
        int pct = (int)lroundf(st->scaffold_progress * 100.0f);
        int held = (int)lroundf(ship_cargo_amount(ship, COMMODITY_FRAME));
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_printf("SCAFFOLD %d%%", pct);
        my += row_h * 1.5f;
        sdtx_color3b(held > 0 ? 130 : 145, held > 0 ? 255 : 160, held > 0 ? 235 : 188);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        if (held > 0)
            sdtx_printf("[S] DELIVER %d frames -> +construction", held);
        else
            sdtx_puts("Bring frames here to finish this outpost.");
        return;
    }

    /* === BUY (F) — the headline verb at any producer station === */
    {
        commodity_t sell = station_primary_sell(st);
        if ((int)sell >= 0) {
            float price_f = station_sell_price(st, sell);
            int   price   = (int)lroundf(price_f);
            float avail   = station_inventory_amount(st, sell);
            float space   = ship_cargo_capacity(ship) - ship_total_cargo(ship);
            float credits = player_current_balance();
            int   afford  = (price_f > FLOAT_EPSILON) ? (int)floorf(credits / price_f) : 0;
            int   can     = (int)fminf(fminf(avail, space), (float)afford);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            if (can > 0) {
                sdtx_color3b(PAL_CONTRACT_AFFORD);
                sdtx_printf("[F] BUY  %d %s   -%d %s",
                            can, commodity_short_name(sell),
                            can * price, ui_station_currency(st));
                sdtx_color3b(PAL_TEXT_FADED);
                sdtx_pos(ui_text_pos(right_x), ui_text_pos(my));
                sdtx_printf("(%d in stock @ %d %s)",
                            (int)lroundf(avail), price, ui_station_currency(st));
                my += row_h;
            } else if (avail > 0.5f) {
                /* Stock exists but you can't take any — explain why */
                sdtx_color3b(PAL_COND_DISABLE_AFFORD);
                if (space < 0.5f)
                    sdtx_printf("[F] BUY  hold full");
                else
                    sdtx_printf("[F] BUY  need %d %s",
                                price, ui_station_currency(st));
                my += row_h;
            }
            /* If avail == 0 we render nothing (no aspirational rows) */
        }
    }

    /* === SELL (S) — only when player has something this station accepts === */
    {
        commodity_t buy = station_primary_buy(st);
        if ((int)buy >= 0) {
            int held = (int)lroundf(ship_cargo_amount(ship, buy));
            if (held > 0) {
                int price = (int)lroundf(station_buy_price(st, buy));
                sdtx_color3b(PAL_CONTRACT_READY);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                sdtx_printf("[S] SELL %d %s   +%d %s",
                            held, commodity_short_name(buy),
                            held * price, ui_station_currency(st));
                my += row_h;
            }
        }
    }

    /* === REPAIR (R) === */
    if (ui->repair_cost > 0 && ui->can_repair) {
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_printf("[R] REPAIR  -%d %s   (hull %d -> %d)",
                    ui->repair_cost, ui_station_currency(st),
                    ui->hull_now, ui->hull_max);
        my += row_h;
    }

    /* === UPGRADES — one row per upgrade only when actionable here === */
    {
        bool m_avail = station_has_module(st, MODULE_LASER_FAB)
                    && !ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING);
        bool h_avail = station_has_module(st, MODULE_FRAME_PRESS)
                    && !ship_upgrade_maxed(ship, SHIP_UPGRADE_HOLD);
        bool t_avail = station_has_module(st, MODULE_TRACTOR_FAB)
                    && !ship_upgrade_maxed(ship, SHIP_UPGRADE_TRACTOR);
        if (m_avail) {
            sdtx_color3b(ui->can_upgrade_mining ? PAL_TEXT_SECONDARY : PAL_AFFORD_INACTIVE);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            sdtx_printf("[M] LASER   +dps   -%d %s",
                        ui->mining_cost, ui_station_currency(st));
            my += row_h;
        }
        if (h_avail) {
            sdtx_color3b(ui->can_upgrade_hold ? PAL_TEXT_SECONDARY : PAL_AFFORD_INACTIVE);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            sdtx_printf("[H] HOLD    +cap   -%d %s",
                        ui->hold_cost, ui_station_currency(st));
            my += row_h;
        }
        if (t_avail) {
            sdtx_color3b(ui->can_upgrade_tractor ? PAL_TEXT_SECONDARY : PAL_AFFORD_INACTIVE);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            sdtx_printf("[T] TRACTOR +rng   -%d %s",
                        ui->tractor_cost, ui_station_currency(st));
            my += row_h;
        }
    }

    my += row_h * 0.5f;

    /* === Sub-screens — only when there's something inside === */
    {
        int jobs_here = 0, jobs_nearby = 0;
        int here_idx = LOCAL_PLAYER.current_station;
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            const contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->station_index >= MAX_STATIONS) continue;
            if (here_idx >= 0 && ct->station_index == here_idx
                && ct->action == CONTRACT_TRACTOR
                && ship->cargo[ct->commodity] >= 0.5f) jobs_here++;
            else jobs_nearby++;
        }
        if (jobs_here + jobs_nearby > 0) {
            sdtx_color3b(jobs_here > 0 ? PAL_CONTRACT_READY : PAL_CONTRACT_HINT);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            if (jobs_here > 0)
                sdtx_printf("[J] JOBS   %d here, %d nearby",
                            jobs_here, jobs_nearby);
            else
                sdtx_printf("[J] JOBS   %d nearby", jobs_nearby);
            my += row_h;
        }
    }

    if (station_has_module(st, MODULE_SHIPYARD)) {
        int buildable = 0;
        uint32_t mask = ship->unlocked_modules;
        for (int i = 0; i < MODULE_COUNT; i++) {
            if (module_kind((module_type_t)i) == MODULE_KIND_NONE) continue;
            if (!station_has_module(st, (module_type_t)i)) continue;
            if (!module_unlocked_for_player(mask, (module_type_t)i)) continue;
            buildable++;
        }
        sdtx_color3b(buildable > 0 ? PAL_HOLD_CYAN : PAL_AFFORD_INACTIVE);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
        sdtx_printf("[B] BUILD  %d module%s",
                    buildable, buildable == 1 ? "" : "s");
        my += row_h;
    }
}

/* ------------------------------------------------------------------ */
/* JOBS sub-screen — preserved CONTRACTS picker, trimmed              */
/* ------------------------------------------------------------------ */

static void draw_jobs_view(const station_ui_state_t *ui,
                           float cx, float cy, float inner_w, bool compact)
{
    (void)compact;
    sdtx_color3b(PAL_HOLD_CYAN);
    sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
    sdtx_puts("JOBS  [Esc] back");
    sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 14.0f));
    sdtx_color3b(PAL_STATION_HINT);
    if (g.selected_contract >= 0)
        sdtx_puts("[E] deliver selected   [1-3] reselect");
    else
        sdtx_puts("[E] deliver all matching   [1-3] select one");

    /* Build slot listing (same logic as the old CONTRACTS tab). */
    int slots[3] = {-1, -1, -1};
    int slot_count = 0;
    bool slot_fulfillable[3] = {false, false, false};
    int slot_held[3] = {0, 0, 0};
    int here_idx = LOCAL_PLAYER.current_station;

    /* Pass 1: contracts here AND we have the cargo. */
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
    /* Pass 2: nearest other contracts (only fill if pass 1 left gaps). */
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
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 32.0f));
        sdtx_color3b(PAL_INSPECT_STATION);
        sdtx_puts("No active contracts.");
        return;
    }

    for (int s = 0; s < slot_count; s++) {
        contract_t *ct = &g.world.contracts[slots[s]];
        float cprice = ct->base_price * (1.0f + fminf(ct->age / 300.0f, 1.0f) * 0.2f);
        float line_y = cy + 32.0f + (float)s * 20.0f;
        bool tracked = (g.tracked_contract == slots[s]);
        bool selected = (g.selected_contract == slots[s]);
        float pip_r = 0.5f, pip_g = 0.5f, pip_b = 0.5f;
        if (ct->action == CONTRACT_FRACTURE) {
            pip_r = 0.95f; pip_g = 0.30f; pip_b = 0.20f;
        } else {
            if      (ct->commodity == COMMODITY_FERRITE_ORE) { pip_r = 0.85f; pip_g = 0.50f; pip_b = 0.35f; }
            else if (ct->commodity == COMMODITY_CUPRITE_ORE) { pip_r = 0.40f; pip_g = 0.55f; pip_b = 0.90f; }
            else if (ct->commodity == COMMODITY_CRYSTAL_ORE) { pip_r = 0.40f; pip_g = 0.85f; pip_b = 0.50f; }
            else                                              { pip_r = 0.60f; pip_g = 0.75f; pip_b = 0.90f; }
        }
        draw_rect_centered(v2(cx + 3.0f, line_y + 5.0f), 3.0f, 3.0f, pip_r, pip_g, pip_b, 0.9f);
        sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(line_y));
        if (selected)               sdtx_color3b(PAL_CONTRACT_PENDING);
        else if (slot_fulfillable[s]) sdtx_color3b(PAL_CONTRACT_READY);
        else if (tracked)            sdtx_color3b(PAL_CONTRACT_STATUS);
        else                         sdtx_color3b(PAL_CONTRACT_HINT);
        const char *marker = selected ? " <" : (tracked ? " *" : "");
        const station_t *dest = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;
        const char *pay_cur = ui_station_currency(dest ? dest : ui->station);
        if (ct->action == CONTRACT_FRACTURE) {
            sdtx_printf("[%d] FRACTURE %.0f %s%s",
                s + 1, cprice, pay_cur, marker);
        } else if (slot_fulfillable[s]) {
            sdtx_printf("[%d] DELIVER %dx %s -> %.0f %s%s",
                s + 1, slot_held[s], commodity_short_name(ct->commodity),
                cprice, pay_cur, marker);
        } else {
            const char *stn = dest ? dest->name : "???";
            int max_name = (int)((inner_w - 48.0f) / 8.0f) - 20;
            if (max_name < 6) max_name = 6;
            sdtx_printf("[%d] %s -> %.*s: %.0f %s%s",
                s + 1, commodity_short_name(ct->commodity),
                max_name, stn,
                cprice, pay_cur, marker);
        }
    }
}

/* ------------------------------------------------------------------ */
/* BUILD sub-screen — preserved SHIPYARD picker, trimmed              */
/* ------------------------------------------------------------------ */

static void draw_build_view(const station_ui_state_t *ui,
                            float cx, float cy, bool compact)
{
    (void)compact;
    sdtx_color3b(PAL_ORE_AMBER);
    sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
    sdtx_puts("BUILD  [Esc] back");
    sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 14.0f));
    sdtx_color3b(PAL_STATION_HINT);
    sdtx_puts("[1-9] order a scaffold kit");

    float ly = cy + 34.0f;
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

    /* Pending orders (unchanged) */
    if (ui->station->pending_scaffold_count > 0) {
        ly += 8.0f;
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
        sdtx_printf("PENDING ORDERS (%d/4)", ui->station->pending_scaffold_count);
        ly += 14.0f;
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
            ly += 12.0f;
        }
    }
}

/* ------------------------------------------------------------------ */
/* draw_station_services -- header band + view dispatch                */
/* ------------------------------------------------------------------ */

void draw_station_services(const station_ui_state_t* ui) {
    if (!LOCAL_PLAYER.docked) return;
    if (!ui->station) return;

    /* Auto-leave BUILD if station has no shipyard (e.g. moved between docks) */
    if (g.station_view == STATION_VIEW_BUILD &&
        !station_has_module(ui->station, MODULE_SHIPYARD)) {
        g.station_view = STATION_VIEW_VERBS;
    }

    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    bool compact = ui_is_compact();

    draw_header_band(ui, panel_x, panel_y, panel_w, compact);

    /* View content begins below the 3-line header. */
    float inner_x = panel_x + 18.0f;
    float inner_w = panel_w - 36.0f;
    float content_top = panel_y + 60.0f;
    float cx = inner_x + 18.0f;
    float cy = content_top + 12.0f;

    switch (g.station_view) {
    case STATION_VIEW_VERBS:
        draw_verbs_view(ui, cx, cy, inner_w, compact);
        break;
    case STATION_VIEW_JOBS:
        draw_jobs_view(ui, cx, cy, inner_w, compact);
        break;
    case STATION_VIEW_BUILD:
        draw_build_view(ui, cx, cy, compact);
        break;
    }

    (void)panel_h;
}
