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
    int ferrite = (int)lroundf(station->ore_buffer[COMMODITY_FERRITE_ORE]);
    int cuprite = (int)lroundf(station->ore_buffer[COMMODITY_CUPRITE_ORE]);
    int crystal = (int)lroundf(station->ore_buffer[COMMODITY_CRYSTAL_ORE]);
    snprintf(text, text_size, "FE %d/%d  CU %d/%d  CR %d/%d", ferrite, cap, cuprite, cap, crystal, cap);
}

void format_ingot_stock_line(const station_t* station, char* text, size_t text_size) {
    int frame = (int)lroundf(station_inventory_amount(station, COMMODITY_FRAME_INGOT));
    int conductor = (int)lroundf(station_inventory_amount(station, COMMODITY_CONDUCTOR_INGOT));
    int lens = (int)lroundf(station_inventory_amount(station, COMMODITY_LENS_INGOT));
    snprintf(text, text_size, "%s %d  %s %d  %s %d",
        commodity_code(COMMODITY_FRAME_INGOT), frame,
        commodity_code(COMMODITY_CONDUCTOR_INGOT), conductor,
        commodity_code(COMMODITY_LENS_INGOT), lens);
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
    if (ui->station == NULL) {
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
    ui->can_sell = station_has_service(STATION_SERVICE_ORE_BUYER) && (ore_total > 0.01f);
    ui->can_repair = station_has_service(STATION_SERVICE_REPAIR) && (repair > 0.0f) && (LOCAL_PLAYER.ship.credits + 0.01f >= repair);
    ui->can_upgrade_mining = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_MINING, STATION_SERVICE_UPGRADE_LASER, ui->mining_cost);
    ui->can_upgrade_hold = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, ui->hold_cost);
    ui->can_upgrade_tractor = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_TRACTOR, STATION_SERVICE_UPGRADE_TRACTOR, ui->tractor_cost);
}

/* ------------------------------------------------------------------ */
/* Station header / market formatting                                  */
/* ------------------------------------------------------------------ */

void format_station_header_badge(const station_ui_state_t* ui, char* text, size_t text_size) {
    if (ui->station == NULL) {
        snprintf(text, text_size, "STATION");
        return;
    }

    snprintf(text, text_size, "%s", station_role_market_title(ui->station));
}

void format_station_market_summary(const station_ui_state_t* ui, bool compact, char* text, size_t text_size) {
    if (ui->station == NULL) {
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
    if (ui->station == NULL) {
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
        int buf = (int)lroundf(ui->station->ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)]);
        int prod = (int)lroundf(ui->station->product_stock[PRODUCT_FRAME]);
        snprintf(text, text_size, "Ingots %d // Frames %d", buf, prod);
    } else if (station_has_module(ui->station, MODULE_LASER_FAB) || station_has_module(ui->station, MODULE_TRACTOR_FAB)) {
        int lsr = (int)lroundf(ui->station->product_stock[PRODUCT_LASER_MODULE]);
        int trc = (int)lroundf(ui->station->product_stock[PRODUCT_TRACTOR_MODULE]);
        snprintf(text, text_size, "LSR %d  TRC %d", lsr, trc);
    } else {
        snprintf(text, text_size, "Signal range: %.0f", ui->station->signal_range);
    }
}

/* ------------------------------------------------------------------ */
/* Service line builders                                               */
/* ------------------------------------------------------------------ */

int build_station_service_lines(const station_ui_state_t* ui, station_service_line_t lines[3]) {
    if (ui->station == NULL) {
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
    float cx = inner_x + 18.0f; /* content text x */
    float cy = content_y + 16.0f; /* content text start y */
    station_tab_t visible_tabs[STATION_TAB_COUNT];
    int tab_count = 0;
    if (ui->station->scaffold) {
        visible_tabs[tab_count++] = STATION_TAB_OVERVIEW;
        visible_tabs[tab_count++] = STATION_TAB_CONSTRUCTION;
    } else {
        visible_tabs[tab_count++] = STATION_TAB_OVERVIEW;
        visible_tabs[tab_count++] = STATION_TAB_SERVICES;
        visible_tabs[tab_count++] = STATION_TAB_ROLE;
        visible_tabs[tab_count++] = STATION_TAB_CONTRACTS;
    }
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
        /* Role-specific label for the ROLE tab */
        module_type_t dom = station_dominant_module(ui->station);
        const char* role_label = "STATION";
        if (dom == MODULE_FURNACE) role_label = "REFINERY";
        else if (dom == MODULE_FRAME_PRESS) role_label = "YARD";
        else if (dom == MODULE_LASER_FAB || dom == MODULE_TRACTOR_FAB) role_label = "BENCH";
        else if (dom == MODULE_SIGNAL_RELAY) role_label = "OUTPOST";

        for (int t = 0; t < tab_count; t++) {
            float tx = inner_x + (float)t * tab_w;
            station_tab_t tid = visible_tabs[t];
            bool active = (g.station_tab == tid);
            sdtx_pos(ui_text_pos(tx + 8.0f), ui_text_pos(tab_bar_y + (compact ? 4.0f : 6.0f)));
            sdtx_color3b(active ? 130 : 100, active ? 255 : 120, active ? 235 : 145);
            switch (tid) {
                case STATION_TAB_OVERVIEW:     sdtx_puts("OVERVIEW"); break;
                case STATION_TAB_SERVICES:     sdtx_puts("SERVICES"); break;
                case STATION_TAB_ROLE:         sdtx_puts(role_label); break;
                case STATION_TAB_CONTRACTS:    sdtx_puts("CONTRACTS"); break;
                case STATION_TAB_CONSTRUCTION: sdtx_puts("BUILD"); break;
                default: break;
            }
        }
    }

    /* ---- Tab content ---- */
    switch (g.station_tab) {

    case STATION_TAB_OVERVIEW: {
        if (ui->station->scaffold) {
            int pct = (int)lroundf(ui->station->scaffold_progress * 100.0f);
            int frames_held = (int)lroundf(LOCAL_PLAYER.ship.cargo[COMMODITY_FRAME_INGOT]);
            float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - ui->station->scaffold_progress);
            int needed_int = (int)lroundf(needed);
            sdtx_color3b(203, 220, 248);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_printf("Progress: %d%%", pct);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 16.0f));
            sdtx_printf("Need %d more frame ingots", needed_int);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 32.0f));
            sdtx_printf("You have: %d frame ingots", frames_held);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 56.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("Dock to auto-deliver frames.");
        } else {
            /* Station welcome -- what this place does */
            sdtx_color3b(203, 220, 248);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            if (station_has_module(ui->station, MODULE_FURNACE)) {
                sdtx_puts("Ore intake and refining hub.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Sell raw ore for credits. Smelts ingots");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 34.0f));
                sdtx_puts("for station production.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 58.0f));
                sdtx_color3b(203, 220, 248);
                sdtx_printf("Haul value: %d cr", ui->payout);
            } else if (station_has_module(ui->station, MODULE_FRAME_PRESS)) {
                sdtx_puts("Frame manufacturing yard.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Upgrades cargo hold capacity.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 34.0f));
                sdtx_puts("Produces frame components from ingots.");
            } else if (station_has_module(ui->station, MODULE_SIGNAL_RELAY)) {
                sdtx_puts("Signal relay outpost.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Extends signal range into new territory.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 44.0f));
                sdtx_color3b(203, 220, 248);
                sdtx_printf("Signal range: %.0f", ui->station->signal_range);
            } else {
                sdtx_puts("Field equipment works.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Upgrades mining laser and tractor beam.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 34.0f));
                sdtx_puts("Fabricates modules from ingots.");
            }
            /* Common: repair status */
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 80.0f));
            sdtx_color3b(ui->repair_cost > 0 ? 255 : 145, ui->repair_cost > 0 ? 180 : 160, ui->repair_cost > 0 ? 80 : 188);
            sdtx_printf("Hull repair: %s", ui->repair_cost > 0 ? "needed" : "nominal");
        }
        break;
    }

    case STATION_TAB_SERVICES: {
        if (ui->station->scaffold) break;
        station_service_line_t service_lines[3] = { 0 };
        int service_line_count = build_station_service_lines(ui, service_lines);
        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("SERVICES");
        float card_gap = compact ? 4.0f : 6.0f;
        float line_h = compact ? 18.0f : 24.0f;
        float first_line_y = cy + 20.0f;
        for (int i = 0; i < service_line_count; i++) {
            float line_y = first_line_y + (float)i * (line_h + card_gap);
            draw_station_service_text_line(cx + 6.0f, line_y + (compact ? 5.0f : 8.0f), &service_lines[i], compact);
        }
        break;
    }

    case STATION_TAB_ROLE: {
        if (ui->station->scaffold) break;
        float meter_x = cx;
        float meter_w = fminf(inner_w - 40.0f, 200.0f);

        if (station_has_module(ui->station, MODULE_FURNACE)) {
            /* Ore prices */
            sdtx_color3b(255, 221, 119);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_printf("FE %d  CU %d  CR %d cr/u",
                (int)lroundf(ui->station->buy_price[COMMODITY_FERRITE_ORE]),
                (int)lroundf(ui->station->buy_price[COMMODITY_CUPRITE_ORE]),
                (int)lroundf(ui->station->buy_price[COMMODITY_CRYSTAL_ORE]));
            /* Haul value */
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 16.0f));
            sdtx_color3b(203, 220, 248);
            sdtx_printf("Haul value: %d cr", ui->payout);
            /* Hopper meters */
            float hy = cy + 38.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(hy));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("HOPPERS");
            hy += 14.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(hy)); sdtx_color3b(180, 140, 120); sdtx_puts("FE");
            draw_ui_meter(meter_x + 24.0f, hy * HUD_CELL + 1.0f, meter_w, 9.0f,
                ui->station->ore_buffer[0] / REFINERY_HOPPER_CAPACITY, 0.72f, 0.42f, 0.28f);
            hy += 16.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(hy)); sdtx_color3b(120, 150, 200); sdtx_puts("CU");
            draw_ui_meter(meter_x + 24.0f, hy * HUD_CELL + 1.0f, meter_w, 9.0f,
                ui->station->ore_buffer[1] / REFINERY_HOPPER_CAPACITY, 0.28f, 0.48f, 0.82f);
            hy += 16.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(hy)); sdtx_color3b(130, 200, 140); sdtx_puts("CR");
            draw_ui_meter(meter_x + 24.0f, hy * HUD_CELL + 1.0f, meter_w, 9.0f,
                ui->station->ore_buffer[2] / REFINERY_HOPPER_CAPACITY, 0.30f, 0.72f, 0.38f);
            /* Ingot stock */
            if (!compact) {
                hy += 22.0f;
                sdtx_pos(ui_text_pos(cx), ui_text_pos(hy));
                sdtx_color3b(145, 160, 188);
                sdtx_printf("Stock  FR %d  CO %d  LN %d",
                    (int)lroundf(ui->station->product_stock[PRODUCT_FRAME]),
                    (int)lroundf(ui->station->product_stock[PRODUCT_TRACTOR_MODULE]),
                    (int)lroundf(ui->station->product_stock[PRODUCT_LASER_MODULE]));
            }
        } else if (station_has_module(ui->station, MODULE_FRAME_PRESS)) {
            sdtx_color3b(130, 255, 235);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_puts("FRAME BAY");
            float fy = cy + 20.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(fy));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("Frame stock");
            draw_ui_meter(meter_x, fy * HUD_CELL + 14.0f, meter_w, 10.0f,
                ui->station->product_stock[PRODUCT_FRAME] / MAX_PRODUCT_STOCK, 0.50f, 0.82f, 1.0f);
            fy += 30.0f;
            int need = (int)lroundf(upgrade_product_cost(&LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD));
            sdtx_pos(ui_text_pos(cx), ui_text_pos(fy));
            sdtx_color3b(203, 220, 248);
            sdtx_printf("Hold upgrade: %s",
                ship_upgrade_maxed(&LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD) ? "MAX" : "available");
            if (!ship_upgrade_maxed(&LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD)) {
                sdtx_pos(ui_text_pos(cx), ui_text_pos(fy + 16.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_printf("Need %d frames + %d cr", need, ship_upgrade_cost(&LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD));
            }
            draw_upgrade_pips(meter_x, fy * HUD_CELL + 34.0f, LOCAL_PLAYER.ship.hold_level, 0.50f, 0.82f, 1.0f);
        } else if (station_has_module(ui->station, MODULE_LASER_FAB) || station_has_module(ui->station, MODULE_TRACTOR_FAB)) {
            sdtx_color3b(130, 255, 235);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_puts("FIELD BENCH");
            float by = cy + 20.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(by));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("Laser modules");
            draw_ui_meter(meter_x, by * HUD_CELL + 14.0f, meter_w, 10.0f,
                ui->station->product_stock[PRODUCT_LASER_MODULE] / MAX_PRODUCT_STOCK, 0.34f, 0.88f, 1.0f);
            by += 30.0f;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(by));
            sdtx_puts("Tractor modules");
            draw_ui_meter(meter_x, by * HUD_CELL + 14.0f, meter_w, 10.0f,
                ui->station->product_stock[PRODUCT_TRACTOR_MODULE] / MAX_PRODUCT_STOCK, 0.42f, 1.0f, 0.86f);
            if (!compact) {
                by += 26.0f;
                draw_upgrade_pips(meter_x, by * HUD_CELL, LOCAL_PLAYER.ship.mining_level, 0.34f, 0.88f, 1.0f);
                draw_upgrade_pips(meter_x, by * HUD_CELL + 14.0f, LOCAL_PLAYER.ship.tractor_level, 0.42f, 1.0f, 0.86f);
            }
        } else if (station_has_module(ui->station, MODULE_SIGNAL_RELAY)) {
            sdtx_color3b(130, 255, 235);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_puts("RELAY STATUS");
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
            sdtx_color3b(203, 220, 248);
            sdtx_printf("Signal range: %.0f", ui->station->signal_range);
        }
        break;
    }

    case STATION_TAB_CONTRACTS: {
        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("CONTRACTS");
        int shown = 0;
        int max_show = compact ? 6 : 8;
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->station_index < 0 || ct->station_index >= MAX_STATIONS) continue;
            if (!station_exists(&g.world.stations[ct->station_index])) continue;
            float cprice = ct->base_price * (1.0f + ct->age / 300.0f * 0.2f);
            if (cprice < 0.01f) continue;
            float line_y = cy + 22.0f + (float)shown * 18.0f;
            /* Commodity color pip */
            float pip_r = 0.5f, pip_g = 0.5f, pip_b = 0.5f;
            if (ct->commodity == COMMODITY_FERRITE_ORE) { pip_r = 0.85f; pip_g = 0.50f; pip_b = 0.35f; }
            else if (ct->commodity == COMMODITY_CUPRITE_ORE) { pip_r = 0.40f; pip_g = 0.55f; pip_b = 0.90f; }
            else if (ct->commodity == COMMODITY_CRYSTAL_ORE) { pip_r = 0.40f; pip_g = 0.85f; pip_b = 0.50f; }
            else if (ct->commodity == COMMODITY_FRAME_INGOT) { pip_r = 0.70f; pip_g = 0.75f; pip_b = 0.90f; }
            else if (ct->commodity == COMMODITY_CONDUCTOR_INGOT) { pip_r = 0.60f; pip_g = 0.80f; pip_b = 1.0f; }
            else if (ct->commodity == COMMODITY_LENS_INGOT) { pip_r = 0.55f; pip_g = 0.95f; pip_b = 0.65f; }
            draw_rect_centered(v2(cx * HUD_CELL + 2.0f, line_y * HUD_CELL + 5.0f), 3.0f, 3.0f, pip_r, pip_g, pip_b, 0.9f);
            /* Age-based brightness: newer = brighter */
            float age_fade = clampf(ct->age / 600.0f, 0.0f, 1.0f);
            uint8_t txt_r = (uint8_t)lerpf(220.0f, 120.0f, age_fade);
            uint8_t txt_g = (uint8_t)lerpf(235.0f, 140.0f, age_fade);
            uint8_t txt_b = (uint8_t)lerpf(255.0f, 170.0f, age_fade);
            sdtx_pos(ui_text_pos(cx + 12.0f), ui_text_pos(line_y));
            sdtx_color3b(txt_r, txt_g, txt_b);
            sdtx_printf("%s @ %s: %.0f cr/u",
                commodity_short_name(ct->commodity),
                g.world.stations[ct->station_index].name,
                cprice);
            if (++shown >= max_show) break;
        }
        if (shown == 0) {
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("No active contracts.");
        }
        break;
    }

    case STATION_TAB_CONSTRUCTION: {
        if (!ui->station->scaffold) break;
        int pct = (int)lroundf(ui->station->scaffold_progress * 100.0f);
        int frames_held = (int)lroundf(LOCAL_PLAYER.ship.cargo[COMMODITY_FRAME_INGOT]);
        float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - ui->station->scaffold_progress);
        int needed_int = (int)lroundf(needed);

        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("CONSTRUCTION");

        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
        sdtx_color3b(255, 221, 119);
        sdtx_printf("Progress: %d%%", pct);

        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 40.0f));
        sdtx_color3b(203, 220, 248);
        sdtx_printf("Materials needed: %d frame ingots", needed_int);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 56.0f));
        sdtx_printf("You carry: %d frame ingots", frames_held);

        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 80.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("Frame ingots auto-deliver on dock.");
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 96.0f));
        sdtx_printf("Signal range on activation: %.0f", OUTPOST_SIGNAL_RANGE);
        break;
    }

    default:
        break;
    }
}
