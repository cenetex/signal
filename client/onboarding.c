/*
 * onboarding.c — First-run guide objectives for Signal Space Miner.
 *
 * This is deliberately not a contract/quest generator. It owns the
 * bottom-right guide/SIGNAL text surface: first the local teaching loop
 * (launch, fly, fracture, tractor, scan), then the concrete next step
 * for whatever station contract or ready ship upgrade the player is
 * tracking.
 */
#include "client.h"
#include "contract_fit.h"
#include "manifest.h"
#include "signal_model.h"  /* SIGNAL_BAND_OPERATIONAL threshold */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ------------------------------------------------------------------ */
/* Persistence (localStorage for browser, no-op for native)           */
/* ------------------------------------------------------------------ */

void onboarding_load(void) {
    if (g.onboarding.loaded) return;
    g.onboarding.loaded = true;
    /* Always start fresh — controls change between versions,
     * so the guide can re-teach bindings every session. */
}

static void onboarding_save(void) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    if (g.onboarding.moved)     flags |= (1 << 0);
    if (g.onboarding.fractured) flags |= (1 << 1);
    if (g.onboarding.tractored) flags |= (1 << 2);
    if (g.onboarding.hailed)    flags |= (1 << 3);
    char js[80];
    snprintf(js, sizeof(js), "localStorage.setItem('signal_onboarding','%d')", flags);
    emscripten_run_script(js);
#endif
}

/* ------------------------------------------------------------------ */
/* Step completion                                                     */
/* ------------------------------------------------------------------ */

static bool onboarding_core_complete(void) {
    return g.onboarding.moved &&
           g.onboarding.fractured &&
           g.onboarding.tractored &&
           g.onboarding.hailed;
}

static void onboarding_refresh_complete(void) {
    g.onboarding.complete = onboarding_core_complete();
}

static void complete_step(bool *step) {
    if (*step) return;
    *step = true;
    onboarding_refresh_complete();
    onboarding_save();
}

void onboarding_mark_moved(void) {
    complete_step(&g.onboarding.moved);
}
void onboarding_mark_fractured(void) {
    complete_step(&g.onboarding.fractured);
}
void onboarding_mark_tractored(void) {
    complete_step(&g.onboarding.tractored);
}
void onboarding_mark_hailed(void) {
    complete_step(&g.onboarding.hailed);
}
void onboarding_mark_boosted(void) {
    complete_step(&g.onboarding.boosted);
}

/* ------------------------------------------------------------------ */
/* Guide objective formatters                                          */
/* ------------------------------------------------------------------ */

typedef bool (*guide_formatter_t)(char *message, size_t message_size);

typedef struct {
    guide_formatter_t format;
} guide_objective_t;

static bool guide_launch(char *message, size_t message_size) {
    if (g.onboarding.moved || !LOCAL_PLAYER.docked) return false;
    snprintf(message, message_size,
             "GUIDE // LAUNCH FROM DOCK ::::: [E]");
    return true;
}

static bool guide_flight(char *message, size_t message_size) {
    if (g.onboarding.moved || LOCAL_PLAYER.docked) return false;
    snprintf(message, message_size,
             "GUIDE // FLIGHT CHECK ::::: [WASD] FLY TOWARD ROCKS");
    return true;
}

static bool guide_fracture(char *message, size_t message_size) {
    if (!g.onboarding.moved || g.onboarding.fractured || LOCAL_PLAYER.docked)
        return false;
    if (LOCAL_PLAYER.hover_asteroid >= 0 &&
        g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        snprintf(message, message_size,
                 "GUIDE // ROCK TARGETED ::::: [M] FRACTURE");
    } else {
        snprintf(message, message_size,
                 "GUIDE // AIM AT A LARGE ROCK ::::: [M] FRACTURE");
    }
    return true;
}

static bool guide_tractor(char *message, size_t message_size) {
    if (!g.onboarding.fractured || g.onboarding.tractored || LOCAL_PLAYER.docked)
        return false;
    if (LOCAL_PLAYER.nearby_fragments > 0) {
        snprintf(message, message_size,
                 "GUIDE // FRAGMENTS NEARBY ::::: HOLD [SPACE] TRACTOR");
    } else {
        snprintf(message, message_size,
                 "GUIDE // BREAK ROCKS INTO FRAGMENTS ::::: THEN [SPACE]");
    }
    return true;
}

static bool guide_scan(char *message, size_t message_size) {
    if (!g.onboarding.tractored || g.onboarding.hailed || LOCAL_PLAYER.docked)
        return false;
    float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
    if (sig > 0.0f) {
        snprintf(message, message_size,
                 "GUIDE // LOCAL SCAN READY ::::: [H] REVEAL IDS + WORK");
    } else {
        snprintf(message, message_size,
                 "GUIDE // RETURN TO SIGNAL ::::: [H] SCANS WHEN LINKED");
    }
    return true;
}

static const guide_objective_t GUIDE_OBJECTIVES[] = {
    { guide_launch },
    { guide_flight },
    { guide_fracture },
    { guide_tractor },
    { guide_scan },
};

/* ------------------------------------------------------------------ */
/* Tracked contract next-step formatter                                */
/* ------------------------------------------------------------------ */

static const contract_t *tracked_contract_ptr(void) {
    if (g.tracked_contract < 0 || g.tracked_contract >= MAX_CONTRACTS)
        return NULL;
    contract_t *ct = &g.world.contracts[g.tracked_contract];
    if (!ct->active) {
        g.tracked_contract = -1;
        return NULL;
    }
    return ct;
}

static void contract_station_name(int station_index, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    if (station_index >= 0 && station_index < MAX_STATIONS) {
        const station_t *st = &g.world.stations[station_index];
        if (station_exists(st) && st->name[0] != '\0') {
            snprintf(out, out_size, "%s", st->name);
            return;
        }
    }
    snprintf(out, out_size, "station");
}

static int contract_quantity_goal(const contract_t *ct) {
    int qty = (ct && ct->quantity_needed > 0.5f)
            ? (int)ceilf(ct->quantity_needed)
            : 1;
    return qty > 0 ? qty : 1;
}

static float contract_towed_matching_ore(const contract_t *ct) {
    float held = 0.0f;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    for (int t = 0; t < ship->towed_count; t++) {
        int fi = ship->towed_fragments[t];
        if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[fi];
        if (contract_fit_is_ok(contract_fit_fragment(ct, a)))
            held += a->ore;
    }
    return held;
}

static bool contract_has_matching_fragment(const contract_t *ct) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (contract_fit_is_ok(contract_fit_fragment(ct, a)))
            return true;
    }
    return false;
}

static int nearest_station_with_stock(commodity_t commodity, vec2 from,
                                      int exclude_station) {
    int best = -1;
    float best_d = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        if (station_finished_count(st, commodity) <= 0) continue;
        float d = v2_dist_sq(from, st->pos);
        if (best < 0 || d < best_d) {
            best = s;
            best_d = d;
        }
    }
    return best;
}

static int nearest_station_producing(commodity_t commodity, vec2 from) {
    int best = -1;
    float best_d = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        if (!station_produces(st, commodity)) continue;
        float d = v2_dist_sq(from, st->pos);
        if (best < 0 || d < best_d) {
            best = s;
            best_d = d;
        }
    }
    return best;
}

static void format_upstream_step(commodity_t commodity,
                                 char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    switch (commodity) {
    case COMMODITY_FERRITE_INGOT:
        snprintf(out, out_size, "MINE FERRITE; SMELT TO FE INGOT");
        break;
    case COMMODITY_CUPRITE_INGOT:
        snprintf(out, out_size, "MINE CUPRITE; SMELT TO CU INGOT");
        break;
    case COMMODITY_CRYSTAL_INGOT:
        snprintf(out, out_size, "MINE CRYSTAL; SMELT TO CR INGOT");
        break;
    case COMMODITY_FRAME:
        snprintf(out, out_size, "MAKE FRAME: FE INGOT TO FRAME PRESS");
        break;
    case COMMODITY_LASER_MODULE:
        snprintf(out, out_size, "MAKE LASER MOD: CU INGOT + FRAME");
        break;
    case COMMODITY_TRACTOR_MODULE:
        snprintf(out, out_size, "MAKE TRACTOR MOD: CR INGOT + FRAME");
        break;
    case COMMODITY_REPAIR_KIT:
        snprintf(out, out_size, "MAKE REPAIR KITS: FRAME + LASER + TRACTOR");
        break;
    default:
        snprintf(out, out_size, "SOURCE %s", commodity_short_name(commodity));
        break;
    }
}

static bool contract_step_fracture(const contract_t *ct,
                                   char *message, size_t message_size) {
    int idx = ct->target_index;
    if (idx >= 0 && idx < MAX_ASTEROIDS) {
        const asteroid_t *a = &g.world.asteroids[idx];
        if (a->active && a->tier != ASTEROID_TIER_S) {
            snprintf(message, message_size,
                     "SIGNAL // NEXT STEP ::::: FRACTURE %s TARGET ASTEROID [M]",
                     commodity_short_name((commodity_t)a->commodity));
            return true;
        }
    }
    if (ct->commodity < COMMODITY_COUNT) {
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: FRACTURE %s ASTEROID [M]",
                 commodity_short_name(ct->commodity));
    } else {
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: FRACTURE TARGET ASTEROID [M]");
    }
    return true;
}

static bool contract_step_raw_tractor(const contract_t *ct,
                                      char *message, size_t message_size) {
    char dest[32];
    contract_station_name(ct->station_index, dest, sizeof(dest));

    if (contract_towed_matching_ore(ct) > 0.0f) {
        const station_t *st = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;
        if (st && station_exists(st) && station_can_smelt(st, ct->commodity)) {
            snprintf(message, message_size,
                     "SIGNAL // NEXT STEP ::::: TOW %s FRAGMENTS TO %s FURNACE BEAM",
                     commodity_short_name(ct->commodity), dest);
        } else {
            snprintf(message, message_size,
                     "SIGNAL // NEXT STEP ::::: TOW %s FRAGMENTS TO COMPATIBLE FURNACE",
                     commodity_short_name(ct->commodity));
        }
        return true;
    }

    if (contract_has_matching_fragment(ct)) {
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: TRACTOR %s FRAGMENTS, THEN FURNACE BEAM",
                 commodity_short_name(ct->commodity));
    } else {
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: FRACTURE %s ASTEROID FOR FURNACE FEED",
                 commodity_short_name(ct->commodity));
    }
    return true;
}

static bool contract_step_finished_delivery(const contract_t *ct,
                                            char *message, size_t message_size) {
    char dest[32];
    contract_station_name(ct->station_index, dest, sizeof(dest));
    int qty = contract_quantity_goal(ct);
    int held = contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest);
    if (held > 0) {
        int deliver = held < qty ? held : qty;
        if (LOCAL_PLAYER.docked &&
            LOCAL_PLAYER.current_station == (int)ct->station_index) {
            if (g.station_view == STATION_VIEW_WORK) {
                snprintf(message, message_size,
                         "SIGNAL // NEXT STEP ::::: DELIVER %s x%d TO %s [S]",
                         commodity_short_name(ct->commodity), deliver, dest);
            } else {
                snprintf(message, message_size,
                         "SIGNAL // NEXT STEP ::::: DELIVER %s x%d TO %s: OPEN JOBS [TAB]",
                         commodity_short_name(ct->commodity), deliver, dest);
            }
        } else {
            snprintf(message, message_size,
                     "SIGNAL // NEXT STEP ::::: DELIVER %s x%d TO %s",
                     commodity_short_name(ct->commodity), deliver, dest);
        }
        return true;
    }

    int stock_station = nearest_station_with_stock(ct->commodity,
                                                   LOCAL_PLAYER.ship.pos,
                                                   (int)ct->station_index);
    if (stock_station >= 0) {
        char stock[32];
        contract_station_name(stock_station, stock, sizeof(stock));
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: PICK UP %s AT %s, DELIVER TO %s",
                 commodity_short_name(ct->commodity), stock, dest);
        return true;
    }

    int producer = nearest_station_producing(ct->commodity,
                                             LOCAL_PLAYER.ship.pos);
    if (producer >= 0) {
        char where[32];
        char upstream[80];
        contract_station_name(producer, where, sizeof(where));
        format_upstream_step(ct->commodity, upstream, sizeof(upstream));
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: %s AT %s",
                 upstream, where);
        return true;
    }

    char upstream[80];
    format_upstream_step(ct->commodity, upstream, sizeof(upstream));
    snprintf(message, message_size,
             "SIGNAL // NEXT STEP ::::: %s", upstream);
    return true;
}

bool contract_step_hint(char *message, size_t message_size) {
    if (message_size == 0) return false;
    message[0] = '\0';
    const contract_t *ct = tracked_contract_ptr();
    if (!ct) return false;

    if (ct->action == CONTRACT_FRACTURE)
        return contract_step_fracture(ct, message, message_size);

    if (ct->action == CONTRACT_TRACTOR) {
        if (ct->commodity < COMMODITY_RAW_ORE_COUNT)
            return contract_step_raw_tractor(ct, message, message_size);
        return contract_step_finished_delivery(ct, message, message_size);
    }

    return false;
}

static bool ship_upgrade_step_hint(char *message, size_t message_size) {
    if (message_size == 0 || !LOCAL_PLAYER.docked) return false;
    const station_t *st = current_station_ptr();
    if (!st || !station_has_module(st, MODULE_DOCK)) return false;

    const struct {
        ship_upgrade_t upgrade;
        const char *label;
        const char *key;
    } checks[] = {
        { SHIP_UPGRADE_MINING,  "UPGRADE LASER",        "[M]" },
        { SHIP_UPGRADE_TRACTOR, "UPGRADE TRACTOR BEAM", "[T]" },
    };

    float balance = player_current_balance();
    for (int i = 0; i < (int)(sizeof(checks) / sizeof(checks[0])); i++) {
        if (!can_afford_upgrade(st, &LOCAL_PLAYER.ship,
                                checks[i].upgrade, balance))
            continue;
        snprintf(message, message_size,
                 "SIGNAL // NEXT STEP ::::: %s AT DOCK %s",
                 checks[i].label, checks[i].key);
        return true;
    }
    return false;
}

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (label_size > 0) label[0] = '\0';
    onboarding_refresh_complete();
    if (g.onboarding.complete) {
        /* One final system line, then station hails own station voice. */
        if (!g.onboarding.welcomed) {
            g.onboarding.welcomed = true;
            snprintf(message, message_size,
                     "GUIDE // LOOP COMPLETE ::::: [H] SCAN // LASER INSPECTS");
            return true;
        }
        if (contract_step_hint(message, message_size))
            return true;
        return ship_upgrade_step_hint(message, message_size);
    }

    /* Contextual, optional: boost is useful, but not part of completing the
     * first economy loop. Show it only when the weak-signal context exists. */
    if (!LOCAL_PLAYER.docked && g.onboarding.moved && !g.onboarding.boosted) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig > 0.0f && sig < SIGNAL_BAND_OPERATIONAL) {
            snprintf(message, message_size,
                     "GUIDE // LOW SIGNAL ::::: [SHIFT] BOOST TOWARD LINK");
            return true;
        }
    }

    if (contract_step_hint(message, message_size))
        return true;
    if (ship_upgrade_step_hint(message, message_size))
        return true;

    for (int i = 0; i < (int)(sizeof(GUIDE_OBJECTIVES) / sizeof(GUIDE_OBJECTIVES[0])); i++) {
        if (GUIDE_OBJECTIVES[i].format(message, message_size))
            return true;
    }

    if (message_size > 0) message[0] = '\0';
    return false;
}
