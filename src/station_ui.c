/*
 * station_ui.c -- Station lookup helpers, formatting, and the docked station
 * services text renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"

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
        case MODULE_FURNACE:     return "REFINERY // ore intake";
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
        case MODULE_FURNACE:     return "ORE BOARD";
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
        case MODULE_FURNACE:
            *r = 0.34f; *g0 = 0.96f; *b = 0.76f; break;
        case MODULE_FRAME_PRESS:
            *r = 0.98f; *g0 = 0.74f; *b = 0.30f; break;
        case MODULE_LASER_FAB:
            *r = 0.42f; *g0 = 0.86f; *b = 1.0f; break;
        case MODULE_TRACTOR_FAB:
            *r = 0.42f; *g0 = 0.86f; *b = 1.0f; break;
        case MODULE_SIGNAL_RELAY:
            *r = 0.72f; *g0 = 0.92f; *b = 0.52f; break;
        default:
            *r = 0.45f; *g0 = 0.85f; *b = 1.0f; break;
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

void format_ore_manifest(char* text, size_t text_size) {
    int ferrite = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship,COMMODITY_FERRITE_ORE));
    int cuprite = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship,COMMODITY_CUPRITE_ORE));
    int crystal = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship,COMMODITY_CRYSTAL_ORE));
    snprintf(text, text_size, "%s %d  %s %d  %s %d",
        commodity_code(COMMODITY_FERRITE_ORE), ferrite,
        commodity_code(COMMODITY_CUPRITE_ORE), cuprite,
        commodity_code(COMMODITY_CRYSTAL_ORE), crystal);
}

void format_ore_hopper_line(const station_t* station, char* text, size_t text_size) {
    int cap = (int)lroundf(REFINERY_HOPPER_CAPACITY);
    int ferrite = (int)lroundf(station->inventory[COMMODITY_FERRITE_ORE]);
    int cuprite = (int)lroundf(station->inventory[COMMODITY_CUPRITE_ORE]);
    int crystal = (int)lroundf(station->inventory[COMMODITY_CRYSTAL_ORE]);
    snprintf(text, text_size, "FE %d/%d  CU %d/%d  CR %d/%d", ferrite, cap, cuprite, cap, crystal, cap);
}

void format_ingot_stock_line(const station_t* station, char* text, size_t text_size) {
    int frame = (int)lroundf(station_inventory_amount(station, COMMODITY_FERRITE_INGOT));
    int conductor = (int)lroundf(station_inventory_amount(station, COMMODITY_CUPRITE_INGOT));
    int lens = (int)lroundf(station_inventory_amount(station, COMMODITY_CRYSTAL_INGOT));
    snprintf(text, text_size, "%s %d  %s %d  %s %d",
        commodity_code(COMMODITY_FERRITE_INGOT), frame,
        commodity_code(COMMODITY_CUPRITE_INGOT), conductor,
        commodity_code(COMMODITY_CRYSTAL_INGOT), lens);
}

void format_refinery_price_line(const station_t* station, char* text, size_t text_size) {
    int ferrite = (int)lroundf(station_buy_price(station, COMMODITY_FERRITE_ORE));
    int cuprite = (int)lroundf(station_buy_price(station, COMMODITY_CUPRITE_ORE));
    int crystal = (int)lroundf(station_buy_price(station, COMMODITY_CRYSTAL_ORE));
    snprintf(text, text_size, "FE %d  CU %d  CR %d", ferrite, cuprite, crystal);
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
    float ore_total = ship_raw_ore_total(&LOCAL_PLAYER.ship);
    float repair = station_repair_cost(&LOCAL_PLAYER.ship, current_station_ptr());
    ui->cargo_units = (int)lroundf(ore_total);
    ui->cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    ui->payout = (int)lroundf(station_cargo_sale_value(&LOCAL_PLAYER.ship, current_station_ptr()));
    ui->repair_cost = (int)lroundf(repair);
    ui->mining_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_MINING);
    ui->hold_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_HOLD);
    ui->tractor_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_TRACTOR);
    ui->can_sell = station_has_service(STATION_SERVICE_ORE_BUYER) && (ore_total > FLOAT_EPSILON);
    ui->can_repair = station_has_service(STATION_SERVICE_REPAIR) && (repair > 0.0f) && (LOCAL_PLAYER.ship.credits + FLOAT_EPSILON >= repair);
    ui->can_upgrade_mining = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_MINING, STATION_SERVICE_UPGRADE_LASER, ui->mining_cost);
    ui->can_upgrade_hold = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, ui->hold_cost);
    ui->can_upgrade_tractor = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_TRACTOR, STATION_SERVICE_UPGRADE_TRACTOR, ui->tractor_cost);
}

/* ------------------------------------------------------------------ */
/* Station header / market formatting                                  */
/* ------------------------------------------------------------------ */

void format_station_header_badge(const station_ui_state_t* ui, char* text, size_t text_size) {
    if (!ui->station) {
        snprintf(text, text_size, "STATION");
        return;
    }

    snprintf(text, text_size, "%s", station_role_market_title(ui->station));
}

void format_station_market_summary(const station_ui_state_t* ui, bool compact, char* text, size_t text_size) {
    if (!ui->station) {
        text[0] = '\0';
        return;
    }

    if (station_has_module(ui->station, MODULE_ORE_BUYER)) {
        char manifest[64] = { 0 };
        format_ore_manifest(manifest, sizeof(manifest));
        if (compact) {
            snprintf(text, text_size, "%s %d/%d", manifest, ui->cargo_units, ui->cargo_capacity);
        } else {
            snprintf(text, text_size, "%s // %d/%d", manifest, ui->cargo_units, ui->cargo_capacity);
        }
    } else if (station_has_module(ui->station, MODULE_FRAME_PRESS)) {
        snprintf(text, text_size, "%s", compact ? "Hull service + hold refit" : "Hull service and hold refits.");
    } else if (station_has_module(ui->station, MODULE_LASER_FAB) || station_has_module(ui->station, MODULE_TRACTOR_FAB)) {
        snprintf(text, text_size, "%s", compact ? "Laser + tractor tuning" : "Laser and tractor tuning.");
    } else {
        snprintf(text, text_size, "%s", compact ? "Signal relay outpost" : "Signal relay outpost.");
    }
}

void format_station_market_detail(const station_ui_state_t* ui, bool compact, char* text, size_t text_size) {
    if (!ui->station) {
        text[0] = '\0';
        return;
    }

    if (station_has_module(ui->station, MODULE_ORE_BUYER)) {
        if (compact) {
            snprintf(text, text_size, "Haul %d cr", ui->payout);
        } else {
            char stock[64] = { 0 };
            format_ingot_stock_line(ui->station, stock, sizeof(stock));
            snprintf(text, text_size, "Value %d cr // Stock %s", ui->payout, stock);
        }
    } else if (station_has_module(ui->station, MODULE_FRAME_PRESS)) {
        int buf = (int)lroundf(ui->station->inventory[COMMODITY_FERRITE_INGOT]);
        int prod = (int)lroundf(ui->station->inventory[COMMODITY_FRAME]);
        snprintf(text, text_size, "Ingots %d // Frames %d", buf, prod);
    } else if (station_has_module(ui->station, MODULE_LASER_FAB) || station_has_module(ui->station, MODULE_TRACTOR_FAB)) {
        int lsr = (int)lroundf(ui->station->inventory[COMMODITY_LASER_MODULE]);
        int trc = (int)lroundf(ui->station->inventory[COMMODITY_TRACTOR_MODULE]);
        snprintf(text, text_size, "LSR %d  TRC %d", lsr, trc);
    } else {
        snprintf(text, text_size, "Signal range: %.0f", ui->station->signal_range);
    }
}

/* ------------------------------------------------------------------ */
/* Service line builders                                               */
/* ------------------------------------------------------------------ */

int build_station_service_lines(const station_ui_state_t* ui, station_service_line_t lines[3]) {
    if (!ui->station) {
        return 0;
    }

    memset(lines, 0, sizeof(station_service_line_t) * 3);

    if (station_has_module(ui->station, MODULE_ORE_BUYER)) {
        lines[0].action = "[1] Sell ore";
        snprintf(lines[0].state, sizeof(lines[0].state), "%s", ui->cargo_units > 0 ? "ready" : "empty");
        lines[0].r = ui->can_sell ? 114 : 169;
        lines[0].g0 = ui->can_sell ? 255 : 179;
        lines[0].b = ui->can_sell ? 192 : 204;

        lines[1].action = "[2] Repair hull";
        if (ui->repair_cost > 0) {
            snprintf(lines[1].state, sizeof(lines[1].state), "%d cr", ui->repair_cost);
            lines[1].r = 255;
            lines[1].g0 = 221;
            lines[1].b = 119;
        } else {
            snprintf(lines[1].state, sizeof(lines[1].state), "nominal");
            lines[1].r = 169;
            lines[1].g0 = 179;
            lines[1].b = 204;
        }
        return 2;
    }

    lines[0].action = "[2] Repair hull";
    if (ui->repair_cost > 0) {
        snprintf(lines[0].state, sizeof(lines[0].state), "%d cr", ui->repair_cost);
        lines[0].r = 255;
        lines[0].g0 = 221;
        lines[0].b = 119;
    } else {
        snprintf(lines[0].state, sizeof(lines[0].state), "nominal");
        lines[0].r = 169;
        lines[0].g0 = 179;
        lines[0].b = 204;
    }

    if (station_has_module(ui->station, MODULE_FRAME_PRESS)) {
        lines[1].action = "[4] Hold racks";
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_HOLD)) {
            snprintf(lines[1].state, sizeof(lines[1].state), "maxed");
            lines[1].r = 169;
            lines[1].g0 = 179;
            lines[1].b = 204;
        } else {
            snprintf(lines[1].state, sizeof(lines[1].state), "%d cr", ui->hold_cost);
            lines[1].r = ui->can_upgrade_hold ? 203 : 169;
            lines[1].g0 = ui->can_upgrade_hold ? 220 : 179;
            lines[1].b = ui->can_upgrade_hold ? 248 : 204;
        }
        return 2;
    }

    int count = 1;
    if (station_has_module(ui->station, MODULE_LASER_FAB)) {
        lines[count].action = "[3] Laser array";
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_MINING)) {
            snprintf(lines[count].state, sizeof(lines[count].state), "maxed");
            lines[count].r = 169;
            lines[count].g0 = 179;
            lines[count].b = 204;
        } else {
            snprintf(lines[count].state, sizeof(lines[count].state), "%d cr", ui->mining_cost);
            lines[count].r = ui->can_upgrade_mining ? 203 : 169;
            lines[count].g0 = ui->can_upgrade_mining ? 220 : 179;
            lines[count].b = ui->can_upgrade_mining ? 248 : 204;
        }
        count++;
    }

    if (station_has_module(ui->station, MODULE_TRACTOR_FAB)) {
        lines[count].action = "[5] Tractor coil";
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_TRACTOR)) {
            snprintf(lines[count].state, sizeof(lines[count].state), "maxed");
            lines[count].r = 169;
            lines[count].g0 = 179;
            lines[count].b = 204;
        } else {
            snprintf(lines[count].state, sizeof(lines[count].state), "%d cr", ui->tractor_cost);
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
    sdtx_color3b(line->r, line->g0, line->b);
    if (compact) {
        sdtx_printf("%-16s %s", line->action, line->state);
    } else {
        sdtx_printf("%-26s %s", line->action, line->state);
    }
}

/* ------------------------------------------------------------------ */
/* draw_station_services -- full tabbed content renderer                */
/* ------------------------------------------------------------------ */

void draw_station_services(const station_ui_state_t* ui) {
    if (!LOCAL_PLAYER.docked) return;

    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    bool compact = ui_is_compact();
    float tab_h = compact ? 16.0f : 20.0f;
    float inner_x = panel_x + 18.0f;
    float inner_y = panel_y + 18.0f;
    float inner_w = panel_w - 36.0f;
    float content_y = inner_y + 32.0f + tab_h + 8.0f;
    float cx = inner_x + 18.0f;
    float cy = content_y + 16.0f;
    station_tab_t visible_tabs[STATION_TAB_COUNT];
    int tab_count = 0;
    visible_tabs[tab_count++] = STATION_TAB_STATUS;
    visible_tabs[tab_count++] = STATION_TAB_MARKET;
    visible_tabs[tab_count++] = STATION_TAB_CONTRACTS;
    float tab_w = fminf(inner_w / (float)tab_count, 120.0f);

    /* Station name + role header */
    sdtx_color3b(232, 241, 255);
    sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 16.0f));
    sdtx_puts(ui->station->name);
    if (ui->station->scaffold) {
        sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(255, 221, 119);
        sdtx_puts("UNDER CONSTRUCTION");
    } else {
        sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(118, 255, 221);
        sdtx_puts(station_role_hub_label(ui->station));
    }

    /* Credits badge (right side of header) */
    if (panel_w >= 480.0f) {
        char header_badge[32] = { 0 };
        format_station_header_badge(ui, header_badge, sizeof(header_badge));
        sdtx_pos(ui_text_pos(panel_x + panel_w - 152.0f), ui_text_pos(panel_y + 16.0f));
        sdtx_color3b(203, 220, 248);
        sdtx_puts(header_badge);
        sdtx_pos(ui_text_pos(panel_x + panel_w - 152.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("Tab: switch  E: launch");
    }

    /* Tab labels */
    {
        float tab_bar_y = inner_y + 32.0f;
        for (int t = 0; t < tab_count; t++) {
            float tx = inner_x + (float)t * tab_w;
            station_tab_t tid = visible_tabs[t];
            bool active = (g.station_tab == tid);
            sdtx_pos(ui_text_pos(tx + 8.0f), ui_text_pos(tab_bar_y + (compact ? 4.0f : 6.0f)));
            sdtx_color3b(active ? 130 : 100, active ? 255 : 120, active ? 235 : 145);
            switch (tid) {
                case STATION_TAB_STATUS:    sdtx_puts("STATUS"); break;
                case STATION_TAB_MARKET:    sdtx_puts("MARKET"); break;
                case STATION_TAB_CONTRACTS: sdtx_puts("CONTRACTS"); break;
                default: break;
            }
        }
    }

    /* ---- Build overlay (drawn on top of any tab) ---- */
    if (g.build_overlay && !ui->station->scaffold) {
        sdtx_color3b(255, 221, 119);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("BUILD");
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 14.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("[W/S] ring  [A/D] port  [1-8] module  [Enter] ring  [Esc] close");
        float ly = cy + 32.0f;
        /* Ring/slot selection */
        bool has_ring = station_has_ring(ui->station, g.build_ring);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
        sdtx_color3b(130, 200, 255);
        if (g.build_slot == -1) {
            if (has_ring)
                sdtx_printf("Ring %d (built) - press A/D to select ports", g.build_ring);
            else
                sdtx_printf("Ring %d - press Enter to build", g.build_ring);
        } else {
            /* Check if slot is occupied */
            bool occupied = false;
            for (int m = 0; m < ui->station->module_count; m++) {
                if (ui->station->modules[m].ring == g.build_ring && ui->station->modules[m].slot == g.build_slot) {
                    occupied = true; break;
                }
            }
            sdtx_printf("Ring %d Port %d %s", g.build_ring, g.build_slot, occupied ? "(occupied)" : "(empty)");
        }
        ly += 18.0f;
        /* In-progress modules */
        for (int i = 0; i < ui->station->module_count; i++) {
            if (!ui->station->modules[i].scaffold) continue;
            int pct = (int)lroundf(ui->station->modules[i].build_progress * 100.0f);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
            sdtx_color3b(255, 221, 119);
            sdtx_printf("Building: %d%%", pct);
            ly += 14.0f;
        }
        if (ly > cy + 52.0f) ly += 4.0f;
        /* Available modules (only when port is selected) */
        if (g.build_slot >= 0 && has_ring) {
            int credits = (int)lroundf(LOCAL_PLAYER.ship.credits);
            if (g.build_ring == 1) {
                /* Ring 1: fixed layout — slot 1 = dock, slot 2 = repair */
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                if (g.build_slot == 0) {
                    sdtx_color3b(120, 130, 150);
                    sdtx_puts("Signal relay (pre-placed)");
                } else if (g.build_slot == 1) {
                    bool can_afford = credits >= 100;
                    sdtx_color3b(can_afford ? 203 : 120, can_afford ? 220 : 130, can_afford ? 248 : 150);
                    sdtx_puts("[1] Dock          100cr + 40 frames");
                } else if (g.build_slot == 2) {
                    bool can_afford = credits >= 100;
                    sdtx_color3b(can_afford ? 203 : 120, can_afford ? 220 : 130, can_afford ? 248 : 150);
                    sdtx_puts("[1] Repair Bay    100cr + 30 frames");
                }
                ly += 14.0f;
            } else {
                /* Ring 2+: full module list */
                static const struct { module_type_t type; const char* name; int key; int frames; int credits; } buildable[] = {
                    { MODULE_FURNACE,        "Furnace (FE)", 1, 60,  200 },
                    { MODULE_FURNACE_CU,     "Furnace (CU)", 2, 100, 400 },
                    { MODULE_FURNACE_CR,     "Furnace (CR)", 3, 140, 600 },
                    { MODULE_FRAME_PRESS,    "Frame Press",  4, 80,  300 },
                    { MODULE_LASER_FAB,      "Laser Fab",    5, 80,  300 },
                    { MODULE_TRACTOR_FAB,    "Tractor Fab",  6, 80,  300 },
                    { MODULE_ORE_BUYER,      "Ore Buyer",    7, 40,  100 },
                    { MODULE_DOCK,           "Dock",         8, 40,  100 },
                    { MODULE_SHIPYARD,       "Shipyard",     9, 120, 500 },
                };
                for (int b = 0; b < 9; b++) {
                    if (ui->station->module_count >= MAX_MODULES_PER_STATION) continue;
                    bool can_afford = credits >= buildable[b].credits;
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                    sdtx_color3b(can_afford ? 203 : 120, can_afford ? 220 : 130, can_afford ? 248 : 150);
                    const char *mat_name = "frames";
                    if (buildable[b].type == MODULE_FURNACE_CU || buildable[b].type == MODULE_LASER_FAB) mat_name = "CU ingots";
                    if (buildable[b].type == MODULE_FURNACE_CR || buildable[b].type == MODULE_TRACTOR_FAB) mat_name = "CR ingots";
                    sdtx_printf("[%d] %-14s %dcr + %d %s", buildable[b].key, buildable[b].name, buildable[b].credits, buildable[b].frames, mat_name);
                    ly += 14.0f;
                }
            }
        }
        return; /* overlay takes over rendering */
    }

    /* ---- Tab content ---- */
    switch (g.station_tab) {

    case STATION_TAB_STATUS: {
        float ly = cy;
        bool compact = ui_is_compact();
        float meter_w = fminf(200.0f, inner_w - 120.0f);
        float right_col = cx + meter_w + 16.0f;
        const ship_t* ship = &LOCAL_PLAYER.ship;

        if (ui->station->scaffold) {
            int pct = (int)lroundf(ui->station->scaffold_progress * 100.0f);
            int held = (int)lroundf(ship_cargo_amount(ship, COMMODITY_FRAME));
            sdtx_color3b(255, 221, 119);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
            sdtx_printf("SCAFFOLD  %d%%", pct);
            ly += 18.0f;
            sdtx_color3b(held > 0 ? 130 : 145, held > 0 ? 255 : 160, held > 0 ? 235 : 188);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
            if (held > 0)
                sdtx_printf("[1] Deliver %d frames", held);
            else
                sdtx_puts("Deliver frames to build.");
        } else {
            /* === SHIP STATUS === */
            {
                sdtx_color3b(100, 120, 145);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                sdtx_puts("SHIP");
                draw_ui_rule(cx, cx + meter_w + 80.0f, ly + 11.0f, 0.12f, 0.22f, 0.34f, 0.45f);
                ly += 16.0f;

                /* Hull meter */
                float hull_frac = ship->hull / ship_max_hull(ship);
                bool hull_low = hull_frac < 0.35f;
                sdtx_color3b(hull_low ? 255 : 203, hull_low ? 160 : 220, hull_low ? 130 : 248);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                sdtx_puts("Hull");
                draw_ui_meter(cx + 48.0f, ly + 1.0f, meter_w - 48.0f, 8.0f, clampf(hull_frac, 0.0f, 1.0f),
                    hull_low ? 0.95f : 0.30f, hull_low ? 0.35f : 0.85f, hull_low ? 0.25f : 0.55f);
                sdtx_color3b(145, 160, 188);
                sdtx_pos(ui_text_pos(right_col), ui_text_pos(ly));
                sdtx_printf("%d/%d", (int)lroundf(ship->hull), (int)lroundf(ship_max_hull(ship)));
                ly += 13.0f;

                /* Cargo meter */
                float cargo_total = ship_total_cargo(ship);
                float cargo_cap = ship_cargo_capacity(ship);
                float cargo_frac = cargo_total / fmaxf(1.0f, cargo_cap);
                bool cargo_full = cargo_frac > 0.95f;
                sdtx_color3b(cargo_full ? 255 : 203, cargo_full ? 200 : 220, cargo_full ? 100 : 248);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                sdtx_puts("Cargo");
                draw_ui_meter(cx + 48.0f, ly + 1.0f, meter_w - 48.0f, 8.0f, clampf(cargo_frac, 0.0f, 1.0f),
                    0.26f, 0.85f, 0.68f);
                sdtx_color3b(145, 160, 188);
                sdtx_pos(ui_text_pos(right_col), ui_text_pos(ly));
                sdtx_printf("%d/%d", (int)lroundf(cargo_total), (int)lroundf(cargo_cap));
                ly += compact ? 16.0f : 18.0f;
            }

            /* === SYSTEMS === */
            {
                sdtx_color3b(100, 120, 145);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                sdtx_puts("SYSTEMS");
                draw_ui_rule(cx, cx + meter_w + 80.0f, ly + 11.0f, 0.12f, 0.22f, 0.34f, 0.45f);
                ly += 16.0f;

                /* Laser */
                {
                    bool maxed = ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING);
                    bool available = station_has_module(ui->station, MODULE_LASER_FAB);
                    sdtx_color3b(180, 195, 215);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                    sdtx_puts("Laser");
                    draw_upgrade_pips(cx + 56.0f, ly, ship->mining_level, 0.34f, 0.88f, 1.0f);
                    sdtx_color3b(120, 140, 165);
                    sdtx_pos(ui_text_pos(cx + 120.0f), ui_text_pos(ly));
                    sdtx_printf("%.0f dps", ship_mining_rate(ship));
                    if (available && !maxed) {
                        int cost = ship_upgrade_cost(ship, SHIP_UPGRADE_MINING);
                        bool afford = can_afford_upgrade(ui->station, ship, SHIP_UPGRADE_MINING, STATION_SERVICE_UPGRADE_LASER, cost);
                        sdtx_color3b(afford ? 130 : 90, afford ? 255 : 105, afford ? 235 : 130);
                        sdtx_pos(ui_text_pos(right_col), ui_text_pos(ly));
                        sdtx_printf("[3] %dcr", cost);
                    }
                    ly += 16.0f;
                }

                /* Hold */
                {
                    bool maxed = ship_upgrade_maxed(ship, SHIP_UPGRADE_HOLD);
                    bool available = station_has_module(ui->station, MODULE_FRAME_PRESS);
                    sdtx_color3b(180, 195, 215);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                    sdtx_puts("Hold");
                    draw_upgrade_pips(cx + 56.0f, ly, ship->hold_level, 0.50f, 0.82f, 1.0f);
                    sdtx_color3b(120, 140, 165);
                    sdtx_pos(ui_text_pos(cx + 120.0f), ui_text_pos(ly));
                    sdtx_printf("%.0f cap", ship_cargo_capacity(ship));
                    if (available && !maxed) {
                        int cost = ship_upgrade_cost(ship, SHIP_UPGRADE_HOLD);
                        bool afford = can_afford_upgrade(ui->station, ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost);
                        sdtx_color3b(afford ? 130 : 90, afford ? 255 : 105, afford ? 235 : 130);
                        sdtx_pos(ui_text_pos(right_col), ui_text_pos(ly));
                        sdtx_printf("[4] %dcr", cost);
                    }
                    ly += 16.0f;
                }

                /* Tractor */
                {
                    bool maxed = ship_upgrade_maxed(ship, SHIP_UPGRADE_TRACTOR);
                    bool available = station_has_module(ui->station, MODULE_TRACTOR_FAB);
                    sdtx_color3b(180, 195, 215);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                    sdtx_puts("Tractor");
                    draw_upgrade_pips(cx + 56.0f, ly, ship->tractor_level, 0.42f, 1.0f, 0.86f);
                    sdtx_color3b(120, 140, 165);
                    sdtx_pos(ui_text_pos(cx + 120.0f), ui_text_pos(ly));
                    sdtx_printf("%.0f rng", ship_tractor_range(ship));
                    if (available && !maxed) {
                        int cost = ship_upgrade_cost(ship, SHIP_UPGRADE_TRACTOR);
                        bool afford = can_afford_upgrade(ui->station, ship, SHIP_UPGRADE_TRACTOR, STATION_SERVICE_UPGRADE_TRACTOR, cost);
                        sdtx_color3b(afford ? 130 : 90, afford ? 255 : 105, afford ? 235 : 130);
                        sdtx_pos(ui_text_pos(right_col), ui_text_pos(ly));
                        sdtx_printf("[5] %dcr", cost);
                    }
                    ly += compact ? 16.0f : 18.0f;
                }
            }

            /* === CARGO HOLD === */
            {
                sdtx_color3b(100, 120, 145);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                sdtx_puts("CARGO HOLD");
                draw_ui_rule(cx, cx + meter_w + 80.0f, ly + 11.0f, 0.12f, 0.22f, 0.34f, 0.45f);
                ly += 16.0f;

                bool has_cargo = false;
                for (int c = 0; c < COMMODITY_COUNT; c++) {
                    int amt = (int)lroundf(ship->cargo[c]);
                    if (amt <= 0) continue;
                    has_cargo = true;
                    float mr, mg, mb;
                    commodity_material_tint(commodity_ore_form((commodity_t)c), &mr, &mg, &mb);
                    draw_rect_centered(v2(cx + 3.0f, ly + 4.0f), 2.5f, 2.5f,
                        fminf(1.0f, mr * 1.6f), fminf(1.0f, mg * 1.6f), fminf(1.0f, mb * 1.6f), 0.8f);
                    sdtx_color3b(180, 195, 215);
                    sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(ly));
                    sdtx_printf("%-12s", commodity_short_name((commodity_t)c));
                    sdtx_color3b(203, 220, 248);
                    sdtx_pos(ui_text_pos(cx + 120.0f), ui_text_pos(ly));
                    sdtx_printf("x%d", amt);
                    ly += 11.0f;
                }
                if (ship->has_scaffold_kit) {
                    has_cargo = true;
                    sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(ly));
                    sdtx_color3b(255, 221, 119);
                    sdtx_puts("Scaffold Kit");
                    ly += 11.0f;
                }
                if (!has_cargo) {
                    sdtx_color3b(90, 105, 130);
                    sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(ly));
                    sdtx_puts("Empty");
                    ly += 11.0f;
                }
                ly += 4.0f;
            }

            /* === ACTIONS === */
            {
                /* Sell/deliver */
                if (ui->payout > 0) {
                    sdtx_color3b(130, 255, 235);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                    sdtx_printf("[1] Deliver  +%d cr", ui->payout);
                } else {
                    sdtx_color3b(90, 105, 130);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                    sdtx_puts("[1] Nothing to deliver");
                }
                /* Repair on same line, right side */
                if (ui->repair_cost > 0) {
                    sdtx_color3b(255, 221, 119);
                    sdtx_pos(ui_text_pos(right_col), ui_text_pos(ly));
                    sdtx_printf("[2] Repair -%dcr", ui->repair_cost);
                }
                ly += 14.0f;

                /* Build / launch hint */
                sdtx_color3b(90, 115, 110);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(ly));
                if (ship->has_scaffold_kit)
                    sdtx_puts("[B] Build  [E] Launch + deploy");
                else if (station_has_module(ui->station, MODULE_BLUEPRINT_DESK))
                    sdtx_printf("[B] Scaffold kit %dcr  [E] Launch", (int)OUTPOST_CREDIT_COST);
                else
                    sdtx_puts("[B] Build module  [E] Launch");
            }
        }
        break;
    }

    case STATION_TAB_MARKET: {
        if (ui->station->scaffold) {
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("Under construction.");
            break;
        }
        float my = cy;
        float meter_w = fminf(200.0f, inner_w - 120.0f);
        float right_col = cx + meter_w + 16.0f;
        bool compact = ui_is_compact();

        /* === SELL TO STATION === */
        {
            commodity_t buy = station_primary_buy(ui->station);
            if ((int)buy >= 0) {
                /* Section label */
                sdtx_color3b(100, 120, 145);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                sdtx_puts("SELL TO STATION");
                draw_ui_rule(cx, cx + meter_w + 80.0f, my + 11.0f, 0.12f, 0.22f, 0.34f, 0.45f);
                my += 16.0f;

                /* Commodity pip + name + price */
                float mr, mg, mb;
                commodity_material_tint(commodity_ore_form(buy), &mr, &mg, &mb);
                draw_rect_centered(v2(cx + 3.0f, my + 5.0f), 3.0f, 3.0f,
                    fminf(1.0f, mr * 1.8f), fminf(1.0f, mg * 1.8f), fminf(1.0f, mb * 1.8f), 0.9f);
                sdtx_color3b(203, 220, 248);
                sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(my));
                sdtx_puts(commodity_short_name(buy));
                int buy_price = (int)lroundf(station_buy_price(ui->station, buy));
                sdtx_color3b(145, 160, 188);
                sdtx_pos(ui_text_pos(right_col), ui_text_pos(my));
                sdtx_printf("%d cr/u", buy_price);
                my += 14.0f;

                /* Inventory meter */
                float capacity = (buy < COMMODITY_RAW_ORE_COUNT) ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
                float inv = station_inventory_amount(ui->station, buy);
                float fill = inv / fmaxf(1.0f, capacity);
                draw_ui_meter(cx, my, meter_w, 8.0f, clampf(fill, 0.0f, 1.0f),
                    fminf(1.0f, mr * 1.5f), fminf(1.0f, mg * 1.5f), fminf(1.0f, mb * 1.5f));
                sdtx_color3b(130, 145, 168);
                sdtx_pos(ui_text_pos(right_col), ui_text_pos(my));
                sdtx_printf("%d/%d", (int)lroundf(inv), (int)lroundf(capacity));
                my += 14.0f;

                /* Action line */
                int held = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship, buy));
                if (held > 0) {
                    sdtx_color3b(130, 255, 235);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                    sdtx_printf("[1] Deliver x%d", held);
                    sdtx_color3b(90, 220, 170);
                    sdtx_pos(ui_text_pos(right_col), ui_text_pos(my));
                    sdtx_printf("+%d cr", held * buy_price);
                } else {
                    sdtx_color3b(90, 105, 130);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                    sdtx_puts("[1] No cargo to deliver");
                }
                my += compact ? 18.0f : 22.0f;
            }
        }

        /* === BUY FROM STATION === */
        {
            commodity_t sell = station_primary_sell(ui->station);
            if ((int)sell >= 0) {
                /* Section label */
                sdtx_color3b(100, 120, 145);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                sdtx_puts("BUY FROM STATION");
                draw_ui_rule(cx, cx + meter_w + 80.0f, my + 11.0f, 0.12f, 0.22f, 0.34f, 0.45f);
                my += 16.0f;

                /* Commodity pip + name + price */
                float mr, mg, mb;
                commodity_material_tint(commodity_ore_form(sell), &mr, &mg, &mb);
                draw_rect_centered(v2(cx + 3.0f, my + 5.0f), 3.0f, 3.0f,
                    fminf(1.0f, mr * 1.8f), fminf(1.0f, mg * 1.8f), fminf(1.0f, mb * 1.8f), 0.9f);
                float price_f = station_sell_price(ui->station, sell);
                int price = (int)lroundf(price_f);
                sdtx_color3b(203, 220, 248);
                sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(my));
                sdtx_puts(commodity_short_name(sell));
                sdtx_color3b(145, 160, 188);
                sdtx_pos(ui_text_pos(right_col), ui_text_pos(my));
                sdtx_printf("%d cr/u", price);
                my += 14.0f;

                /* Stock meter */
                float capacity = (sell < COMMODITY_RAW_ORE_COUNT) ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
                float avail = station_inventory_amount(ui->station, sell);
                float fill = avail / fmaxf(1.0f, capacity);
                draw_ui_meter(cx, my, meter_w, 8.0f, clampf(fill, 0.0f, 1.0f),
                    fminf(1.0f, mr * 1.5f), fminf(1.0f, mg * 1.5f), fminf(1.0f, mb * 1.5f));
                sdtx_color3b(130, 145, 168);
                sdtx_pos(ui_text_pos(right_col), ui_text_pos(my));
                sdtx_printf("%d/%d", (int)lroundf(avail), (int)lroundf(capacity));
                my += 14.0f;

                /* Action line */
                float player_space = ship_cargo_capacity(&LOCAL_PLAYER.ship) - ship_total_cargo(&LOCAL_PLAYER.ship);
                float player_credits = LOCAL_PLAYER.ship.credits;
                int can_buy = (avail > 0.5f && price_f > FLOAT_EPSILON)
                    ? (int)fminf(fminf(avail, player_space), floorf(player_credits / price_f))
                    : 0;
                if (can_buy > 0) {
                    sdtx_color3b(130, 255, 235);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                    sdtx_printf("[F] Buy x%d", can_buy);
                    sdtx_color3b(220, 160, 100);
                    sdtx_pos(ui_text_pos(right_col), ui_text_pos(my));
                    sdtx_printf("-%d cr", can_buy * price);
                } else if ((int)lroundf(avail) > 0) {
                    sdtx_color3b(90, 105, 130);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                    sdtx_puts(player_space < 0.5f ? "[F] Hold full" : "[F] Cannot afford");
                } else {
                    sdtx_color3b(90, 105, 130);
                    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                    sdtx_puts("[F] Out of stock");
                }
                my += compact ? 18.0f : 22.0f;
            }
        }

        /* === COMMODITIES LEDGER === */
        if (!compact) {
            sdtx_color3b(85, 100, 120);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            sdtx_puts("COMMODITIES");
            draw_ui_rule(cx, cx + meter_w + 80.0f, my + 11.0f, 0.08f, 0.15f, 0.22f, 0.30f);
            my += 16.0f;

            float mini_w = fminf(90.0f, meter_w * 0.5f);
            for (int c = 0; c < COMMODITY_COUNT; c++) {
                float inv = station_inventory_amount(ui->station, (commodity_t)c);
                float base = ui->station->base_price[c];
                if (inv < 0.5f && base < FLOAT_EPSILON) continue;

                float cap = (c < COMMODITY_RAW_ORE_COUNT) ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
                float fill = inv / fmaxf(1.0f, cap);
                float mr, mg, mb;
                commodity_material_tint(commodity_ore_form((commodity_t)c), &mr, &mg, &mb);

                /* Pip + name */
                draw_rect_centered(v2(cx + 3.0f, my + 4.0f), 2.5f, 2.5f,
                    fminf(1.0f, mr * 1.6f), fminf(1.0f, mg * 1.6f), fminf(1.0f, mb * 1.6f), 0.8f);
                sdtx_color3b(150, 165, 185);
                sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(my));
                sdtx_printf("%-10s", commodity_code((commodity_t)c));

                /* Mini meter */
                float bar_x = cx + 44.0f;
                draw_ui_meter(bar_x, my + 1.0f, mini_w, 6.0f, clampf(fill, 0.0f, 1.0f),
                    fminf(1.0f, mr * 1.3f), fminf(1.0f, mg * 1.3f), fminf(1.0f, mb * 1.3f));

                /* Amount */
                sdtx_color3b(130, 145, 168);
                sdtx_pos(ui_text_pos(bar_x + mini_w + 8.0f), ui_text_pos(my));
                sdtx_printf("%3d", (int)lroundf(inv));

                /* Base price */
                if (base > FLOAT_EPSILON) {
                    sdtx_color3b(100, 115, 138);
                    sdtx_pos(ui_text_pos(bar_x + mini_w + 44.0f), ui_text_pos(my));
                    sdtx_printf("@%d", (int)lroundf(base));
                }
                my += 11.0f;
            }
        }
        break;
    }

    case STATION_TAB_CONTRACTS: {
        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("CONTRACTS");
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 14.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("Press 1-3 to track");

        /* Find top 3 nearest contracts by distance from current station */
        int nearest[3] = {-1, -1, -1};
        float nearest_d[3] = {1e18f, 1e18f, 1e18f};
        vec2 here = ui->station->pos;
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->station_index >= MAX_STATIONS) continue;
            if (!station_exists(&g.world.stations[ct->station_index])) continue;
            vec2 target = (ct->action == CONTRACT_SUPPLY) ? g.world.stations[ct->station_index].pos : ct->target_pos;
            float d = v2_dist_sq(here, target);
            for (int slot = 0; slot < 3; slot++) {
                if (d < nearest_d[slot]) {
                    /* Shift down */
                    for (int j = 2; j > slot; j--) { nearest[j] = nearest[j-1]; nearest_d[j] = nearest_d[j-1]; }
                    nearest[slot] = ci;
                    nearest_d[slot] = d;
                    break;
                }
            }
        }

        int shown = 0;
        for (int slot = 0; slot < 3; slot++) {
            if (nearest[slot] < 0) continue;
            contract_t *ct = &g.world.contracts[nearest[slot]];
            float cprice = ct->base_price * (1.0f + fminf(ct->age / 300.0f, 1.0f) * 0.2f);
            float line_y = cy + 32.0f + (float)shown * 20.0f;
            bool tracked = (g.tracked_contract == nearest[slot]);
            /* Action-based pip color */
            float pip_r = 0.5f, pip_g = 0.5f, pip_b = 0.5f;
            if (ct->action == CONTRACT_DESTROY) { pip_r = 0.95f; pip_g = 0.30f; pip_b = 0.20f; }
            else if (ct->action == CONTRACT_SCAN) { pip_r = 0.30f; pip_g = 0.70f; pip_b = 0.95f; }
            else {
                if (ct->commodity == COMMODITY_FERRITE_ORE) { pip_r = 0.85f; pip_g = 0.50f; pip_b = 0.35f; }
                else if (ct->commodity == COMMODITY_CUPRITE_ORE) { pip_r = 0.40f; pip_g = 0.55f; pip_b = 0.90f; }
                else if (ct->commodity == COMMODITY_CRYSTAL_ORE) { pip_r = 0.40f; pip_g = 0.85f; pip_b = 0.50f; }
                else { pip_r = 0.60f; pip_g = 0.75f; pip_b = 0.90f; }
            }
            draw_rect_centered(v2(cx + 3.0f, line_y + 5.0f), 3.0f, 3.0f, pip_r, pip_g, pip_b, 0.9f);
            sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(line_y));
            sdtx_color3b(tracked ? 255 : 203, tracked ? 255 : 220, tracked ? 130 : 248);
            if (ct->action == CONTRACT_DESTROY) {
                sdtx_printf("[%d] DESTROY: %.0f cr%s", shown + 1, cprice, tracked ? " *" : "");
            } else if (ct->action == CONTRACT_SCAN) {
                sdtx_printf("[%d] SCAN: %.0f cr%s", shown + 1, cprice, tracked ? " *" : "");
            } else {
                sdtx_printf("[%d] %s @ %s: %.0f cr%s", shown + 1,
                    commodity_short_name(ct->commodity),
                    g.world.stations[ct->station_index].name,
                    cprice, tracked ? " *" : "");
            }
            shown++;
        }
        if (shown == 0) {
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("No active contracts.");
        }
        break;
    }

    default:
        break;
    }
}
