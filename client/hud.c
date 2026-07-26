/*
 * hud.c -- HUD layout, drawing primitives, and the main HUD text
 * renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "net.h"
#include "net_sync.h"
#include "inspect_anim.h"
#include "inspect_labels.h"
#include "onboarding.h"
#include "world_draw.h"
#include "avatar.h"
#include "mining_client.h"
#include "mining.h"  /* mining_alphanumeric_callsign — pubkey-derived */
#include "manifest.h"
#include "module_schema.h"
#include "signal_model.h"
#include "palette.h"
#include "contract_fit.h"
#include "npc_identity.h"
#include "ui_clarity.h"
#include "rock_usefulness.h"
#include "chain_log.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define HUD_LATENCY_WARN_MS 100.0f
#define HUD_LATENCY_BAD_MS 300.0f
#define HUD_FRAGMENT_NEARBY_RANGE 220.0f

static void hud_append_text(char *out, size_t cap, const char *text) {
    if (!out || cap == 0 || !text) return;
    size_t len = strlen(out);
    if (len >= cap) return;
    snprintf(out + len, cap - len, "%s", text);
}

static void hud_fit_text(const char *src, int max_chars,
                         char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!src || max_chars <= 0) return;
    size_t src_len = strlen(src);
    size_t limit = (size_t)max_chars;
    if (limit >= cap) limit = cap - 1;
    if (src_len <= limit) {
        snprintf(out, cap, "%s", src);
        return;
    }
    if (limit <= 3) {
        for (size_t i = 0; i < limit; i++) out[i] = '.';
        out[limit] = '\0';
        return;
    }
    memcpy(out, src, limit - 3);
    memcpy(out + limit - 3, "...", 3);
    out[limit] = '\0';
}

#ifdef __EMSCRIPTEN__
EM_JS(int, signal_relay_debug_region_js, (char *out, int cap), {
    if (cap <= 0) return 0;
    var dbg = window.SIGNAL_RELAY_DEBUG || {};
    var value = dbg.region || "";
    var preferred = dbg.preferredRegion || "";
    if (value && preferred && value !== preferred) value = value + "<-" + preferred;
    stringToUTF8(String(value), out, cap);
    return lengthBytesUTF8(String(value));
})

EM_JS(int, signal_relay_debug_broker_latency_ms_js, (), {
    var dbg = window.SIGNAL_RELAY_DEBUG || {};
    var ms = Number(dbg.brokerLatencyMs || 0);
    return (isFinite(ms) && ms > 0) ? Math.round(ms) : 0;
})

EM_JS(int, signal_relay_debug_health_latency_ms_js, (), {
    var dbg = window.SIGNAL_RELAY_DEBUG || {};
    var ms = Number(dbg.healthLatencyMs || 0);
    return (isFinite(ms) && ms > 0) ? Math.round(ms) : 0;
})
#endif

/* ------------------------------------------------------------------ */
/* Station-local balance helper                                        */
/* ------------------------------------------------------------------ */

/* Returns the player's credit balance at the given station. Network authority
 * uses the cached value from the last PLAYER_SHIP message; the offline
 * fallback reads the mirrored ledger. */
static float client_station_balance(int station_idx) {
    if (g.net_authority_enabled)
        return g.station_balance;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return 0.0f;
    const station_t *st = &g.world.stations[station_idx];
    const uint8_t *token = g.world.players[g.local_player_slot].session_token;
    uint8_t pseudo[32];
    client_session_pseudo_pubkey(token, pseudo);
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_pubkey, pseudo, 32) == 0)
            return st->ledger[i].balance;
    }
    return 0.0f;
}

/* Balance at the player's current docked/nearby station. */
float player_current_balance(void) {
    int st = LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station;
    return client_station_balance(st);
}

/* Forward decl — defined below near the other client_* helpers.
 * Fills `strongest_idx` with the station holding the largest pending
 * balance for the local player, `strongest_balance` with that amount,
 * and `other_count` with how many *other* stations also have a
 * positive balance. Returns true when there's any station-local balance to
 * show. Network authority uses the recipient-scoped known-ledger snapshot. */
static bool client_ledger_balance_summary(int *strongest_idx,
                                          float *strongest_balance,
                                          int *other_count);

/* Center `text` horizontally around screen-pixel x = `center_x` and
 * place its baseline at row index `row_idx` (already in canvas-cell
 * units). Cell is the sdtx character width (8 px today). Picks up
 * the current sdtx_color. Used by full-screen overlays that center
 * text and were doing the (cx - w*0.5)/cell math by hand at every
 * call. */
static void sdtx_centered_text(float center_x, float row_idx, float cell, const char *text) {
    if (!text) return;
    float w = (float)strlen(text) * cell;
    sdtx_pos((center_x - w * 0.5f) / cell, row_idx);
    sdtx_puts(text);
}

static uint32_t hud_alpha_pipeline_id;

static void hud_draw_alpha_rect(float x, float y, float width, float height,
                                float r, float g0, float b, float a) {
    if (width <= 0.0f || height <= 0.0f || a <= 0.0f) return;
    if (hud_alpha_pipeline_id == 0) {
        sgl_pipeline pip = sgl_make_pipeline(&(sg_pipeline_desc){
            .colors[0] = {
                .write_mask = SG_COLORMASK_RGBA,
                .blend = {
                    .enabled = true,
                    .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                    .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .src_factor_alpha = SG_BLENDFACTOR_ONE,
                    .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                },
            },
        });
        hud_alpha_pipeline_id = pip.id;
    }

    sgl_push_pipeline();
    sgl_load_pipeline((sgl_pipeline){ hud_alpha_pipeline_id });
    sgl_begin_quads();
    sgl_c4f(r, g0, b, a);
    sgl_v2f(x, y);
    sgl_v2f(x + width, y);
    sgl_v2f(x + width, y + height);
    sgl_v2f(x, y + height);
    sgl_end();
    sgl_pop_pipeline();
}

/* ------------------------------------------------------------------ */
/* Action row classification — single priority chain shared by the     */
/* compact and wide HUDs. hud_classify_action() inspects player +      */
/* world state once and returns a tagged payload; the two renderers    */
/* (hud_render_action_compact / _wide) format it. This eliminates the  */
/* duplicate state machine and fixes the divergence that left towing  */
/* invisible in the wide HUD's action row.                             */
/* ------------------------------------------------------------------ */

typedef enum {
    HUD_ACTION_DOCKED = 0,
    HUD_ACTION_TARGET_ASTEROID,
    HUD_ACTION_SCAN_MODULE,        /* str_a = station name, str_b = module name (or NULL = core hub) */
    HUD_ACTION_SCAN_NPC,           /* str_a = "miner"/"hauler", int_a = total cargo */
    HUD_ACTION_SCAN_PILOT,         /* int_a = pilot id, int_b = hull, str_a = callsign when known */
    HUD_ACTION_SCAN_CARGO_POD,     /* commodity/grade/int_a quantity, int_b = cargo_pod_kind_t */
    HUD_ACTION_MINING,             /* claim window after fracture */
    HUD_ACTION_TOWING,             /* int_a = total tow, int_b = tractor_active,
                                    * tier = fragments, commodity = cargo pods */
    HUD_ACTION_TRACTOR_LOCK,       /* int_a = tractor_fragments, int_b = nearby_fragments */
    HUD_ACTION_TRACTOR_REACHING,   /* tractor active, no frag yet — int_b = nearby_fragments */
    HUD_ACTION_FRAGMENTS_NEARBY,   /* tractor inactive, frags in range — int_b = nearby_fragments */
    HUD_ACTION_HOLD_FULL,
    HUD_ACTION_LEDGER_BALANCE,     /* int_a = strongest station balance,
                                    * int_b = other stations with balance,
                                    * str_a = strongest station currency */
    HUD_ACTION_IDLE,
} hud_action_kind_t;

typedef struct {
    hud_action_kind_t kind;
    int int_a, int_b;
    const char *str_a, *str_b, *str_c;
    /* Asteroid target: separate fields so renderers can format their
     * own short/long flavor of the same data. */
    int tier;            /* asteroid_tier_t */
    int commodity;       /* commodity_t */
    int grade;           /* mining_grade_t, for fragment text color */
    char reason[96];     /* owned copy: safe across return-by-value */
    bool reason_has_color;
    uint8_t reason_color[3];
} hud_action_t;

typedef struct {
    rock_usefulness_candidate_t candidate;
    char label[96];
    bool has_color;
    uint8_t color[3];
} hud_usefulness_t;

static uint8_t hud_best_towed_fragment_grade(void) {
    uint8_t best = (uint8_t)MINING_GRADE_COMMON;
    for (int t = 0; t < LOCAL_PLAYER.ship->towed_count; t++) {
        int idx = LOCAL_PLAYER.ship->towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[idx];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        if (a->grade < (uint8_t)MINING_GRADE_COUNT && a->grade > best)
            best = a->grade;
    }
    return best;
}

static uint8_t hud_best_nearby_fragment_grade(void) {
    uint8_t best = (uint8_t)MINING_GRADE_COMMON;
    float range_sq = HUD_FRAGMENT_NEARBY_RANGE * HUD_FRAGMENT_NEARBY_RANGE;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        if (v2_dist_sq(a->pos, LOCAL_PLAYER.ship->pos) > range_sq) continue;
        if (a->grade < (uint8_t)MINING_GRADE_COUNT && a->grade > best)
            best = a->grade;
    }
    return best;
}

static void hud_set_grade_color(uint8_t grade) {
    uint8_t r, g0, b;
    if (grade >= (uint8_t)MINING_GRADE_COUNT)
        grade = (uint8_t)MINING_GRADE_COMMON;
    mining_grade_rgb((mining_grade_t)grade, &r, &g0, &b);
    sdtx_color3b(r, g0, b);
}

static bool hud_market_memory_from_item(const knowledge_item_t *item,
                                        market_memory_t *out) {
    if (!item || !out || item->kind != (uint8_t)KNOW_MARKET ||
        item->payload_kind != (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY) {
        return false;
    }
    memcpy(out, item->payload, sizeof(*out));
    return out->active &&
           out->memory_kind != (uint8_t)MARKET_MEMORY_NONE;
}

static bool hud_positive_route_memory(const market_memory_t *memory,
                                      commodity_t fragment_commodity) {
    if (!memory || !memory->active) return false;
    if (memory->memory_kind != (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS &&
        memory->memory_kind != (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT &&
        memory->memory_kind != (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION) {
        return false;
    }
    if (memory->station_a >= g.world.station_count ||
        memory->station_b >= g.world.station_count ||
        memory->station_a == memory->station_b) {
        return false;
    }
    if (!station_is_active(&g.world.stations[memory->station_a]) ||
        !station_is_active(&g.world.stations[memory->station_b])) {
        return false;
    }
    commodity_t refined = commodity_refined_form(fragment_commodity);
    return memory->commodity == (uint8_t)fragment_commodity ||
           memory->commodity == (uint8_t)refined;
}

static void hud_copy_station_short_name(int station_idx,
                                        char *out, size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", station_short_name(station_idx));
}

static hud_usefulness_t hud_asteroid_usefulness(const asteroid_t *a) {
    hud_usefulness_t out = {0};
    out.candidate.station_a = -1;
    out.candidate.station_b = -1;
    out.candidate.commodity = COMMODITY_COUNT;
    if (!a || !a->active) return out;

    if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
        const contract_t *ct = &g.world.contracts[g.tracked_contract];
        if (ct->active &&
            contract_fit_is_ok(contract_fit_asteroid(ct, a))) {
            rock_usefulness_candidate_t candidate = {
                .kind = ROCK_USEFULNESS_TRACKED_CONTRACT,
                .station_a = ct->station_index < MAX_STATIONS
                    ? ct->station_index : -1,
                .station_b = -1,
                .commodity = a->commodity,
            };
            rock_usefulness_select(&out.candidate, &candidate);
        }
    }

    if (a->tier == ASTEROID_TIER_S && a->commodity < COMMODITY_COUNT) {
        int station_count = g.world.station_count;
        if (station_count < 0) station_count = 0;
        if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
        for (int i = 0; i < station_count; i++) {
            const station_t *station = &g.world.stations[i];
            if (!station_is_active(station)) continue;
            station_demand_t demand =
                station_demand_for(station, a->commodity);
            if (demand.severity >= 0.20f) {
                rock_usefulness_candidate_t candidate = {
                    .kind = ROCK_USEFULNESS_DIRECT_DEMAND,
                    .station_a = i,
                    .station_b = -1,
                    .commodity = a->commodity,
                    .strength = (uint16_t)lroundf(demand.severity * 1000.0f),
                };
                rock_usefulness_select(&out.candidate, &candidate);
            }
        }

        /* This is the player's bounded carried view: shared directly in local
         * simulation and privately mirrored by PLAYER_MARKET_MEMORIES online.
         * Never read station-global route logs from the flight HUD. */
        {
            const knowledge_view_t *view = &LOCAL_PLAYER.ship->knowledge;
            int item_count = view->count;
            if (item_count > KNOWLEDGE_VIEW_MAX_CAP)
                item_count = KNOWLEDGE_VIEW_MAX_CAP;
            for (int i = 0; i < item_count; i++) {
                market_memory_t memory = {0};
                const knowledge_item_t *item = &view->items[i];
                if (!hud_market_memory_from_item(item, &memory) ||
                    !hud_positive_route_memory(&memory, a->commodity)) {
                    continue;
                }
                rock_usefulness_candidate_t candidate = {
                    .kind = ROCK_USEFULNESS_REMEMBERED_ROUTE,
                    .station_a = memory.station_a,
                    .station_b = memory.station_b,
                    .commodity = memory.commodity,
                    .confidence = item->confidence,
                    .salience = item->salience,
                    .hops = item->hops,
                    .subject_nonce = memory.subject_nonce,
                };
                rock_usefulness_select(&out.candidate, &candidate);
            }
        }

        float nearest_smelt_dist_sq = 0.0f;
        for (int i = 0; i < station_count; i++) {
            const station_t *station = &g.world.stations[i];
            if (!station_is_active(station)) continue;
            for (int m = 0; m < station->module_count; m++) {
                const station_module_t *module = &station->modules[m];
                if (module->scaffold || module->type != MODULE_FURNACE ||
                    module_instance_input_ore(module) != a->commodity) {
                    continue;
                }
                commodity_t refined = module_instance_output(module);
                if (refined >= COMMODITY_COUNT) continue;
                float dist_sq = v2_dist_sq(LOCAL_PLAYER.ship->pos,
                                           station->pos);
                if (out.candidate.kind == ROCK_USEFULNESS_SMELT_PATH &&
                    dist_sq >= nearest_smelt_dist_sq) {
                    continue;
                }
                nearest_smelt_dist_sq = dist_sq;
                rock_usefulness_candidate_t candidate = {
                    .kind = ROCK_USEFULNESS_SMELT_PATH,
                    .station_a = i,
                    .station_b = -1,
                    .commodity = refined,
                    .strength = 1,
                };
                /* Replace an equally ranked smelter when this one is nearer;
                 * higher-ranked demand/route candidates remain untouched. */
                if (out.candidate.kind == ROCK_USEFULNESS_SMELT_PATH)
                    out.candidate = candidate;
                else
                    rock_usefulness_select(&out.candidate, &candidate);
            }
        }
    }

    if (a->tier == ASTEROID_TIER_S &&
        a->grade >= (uint8_t)MINING_GRADE_RARE) {
        rock_usefulness_candidate_t candidate = {
            .kind = ROCK_USEFULNESS_RARE_GRADE,
            .station_a = -1,
            .station_b = -1,
            .commodity = a->commodity,
            .strength = a->grade,
        };
        rock_usefulness_select(&out.candidate, &candidate);
    }

    char station_a[20] = "station";
    char station_b[20] = "station";
    if (out.candidate.station_a >= 0)
        hud_copy_station_short_name(out.candidate.station_a,
                                    station_a, sizeof(station_a));
    if (out.candidate.station_b >= 0)
        hud_copy_station_short_name(out.candidate.station_b,
                                    station_b, sizeof(station_b));

    switch (out.candidate.kind) {
    case ROCK_USEFULNESS_TRACKED_CONTRACT:
        snprintf(out.label, sizeof(out.label), "fits %s work", station_a);
        break;
    case ROCK_USEFULNESS_DIRECT_DEMAND:
        snprintf(out.label, sizeof(out.label), "%s at %s",
                 out.candidate.strength >= 650 ? "needed" : "wanted",
                 station_a);
        break;
    case ROCK_USEFULNESS_REMEMBERED_ROUTE: {
        const uint8_t hi[3] = { PAL_CONTRACT_READY };
        const uint8_t lo[3] = { PAL_TEXT_FADED };
        ui_clarity_t clarity = ui_clarity_from_evidence(
            out.candidate.confidence, out.candidate.salience,
            out.candidate.hops, hi, lo);
        char source_seen[20];
        char dest_seen[20];
        uint32_t seed = (uint32_t)(out.candidate.subject_nonce ^
                                   (out.candidate.subject_nonce >> 32));
        ui_clarity_degrade_text(station_b, clarity.clarity, seed,
                                source_seen, sizeof(source_seen));
        ui_clarity_degrade_text(station_a, clarity.clarity,
                                seed ^ 0x9e3779b9u,
                                dest_seen, sizeof(dest_seen));
        snprintf(out.label, sizeof(out.label), "route remembers %s>%s",
                 source_seen, dest_seen);
        out.has_color = true;
        memcpy(out.color, clarity.fg, sizeof(out.color));
        break;
    }
    case ROCK_USEFULNESS_SMELT_PATH:
        snprintf(out.label, sizeof(out.label), "smelts to %s at %s",
                 commodity_short_name((commodity_t)out.candidate.commodity),
                 station_a);
        break;
    case ROCK_USEFULNESS_RARE_GRADE:
        snprintf(out.label, sizeof(out.label), "%s grade",
                 mining_grade_label((mining_grade_t)a->grade));
        break;
    case ROCK_USEFULNESS_NONE:
    default:
        break;
    }
    return out;
}

static int hud_required_mining_level_for_tier(asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return 3;
    case ASTEROID_TIER_XL:  return 2;
    case ASTEROID_TIER_L:   return 1;
    default:                return 0;
    }
}

static const char *hud_asteroid_gate_reason(const asteroid_t *a) {
    static char label[80];
    label[0] = '\0';
    if (!a || !a->active || asteroid_is_collectible(a)) return NULL;
    int mining_level = LOCAL_PLAYER.ship->mining_level;
    int material_level = mining_required_level_for_commodity(a->commodity);
    int size_level = hud_required_mining_level_for_tier((asteroid_tier_t)a->tier);
    if (mining_level >= material_level && mining_level >= size_level)
        return NULL;

    int required = material_level > size_level ? material_level : size_level;
    const char *laser = required == 0 ? "L1" :
                        required == 1 ? "L2" :
                        required == 2 ? "L3" :
                        required == 3 ? "L4" : "L5";
    if (material_level >= size_level && mining_level < material_level) {
        snprintf(label, sizeof(label), "needs %s laser for %s",
                 laser, commodity_short_name((commodity_t)a->commodity));
    } else {
        snprintf(label, sizeof(label), "needs %s laser for %s %s",
                 laser,
                 asteroid_tier_name((asteroid_tier_t)a->tier),
                 asteroid_tier_kind((asteroid_tier_t)a->tier));
    }
    return label;
}

static const char *hud_module_consequence(const station_t *st, int module_idx) {
    static char label[112];
    label[0] = '\0';
    if (!st || module_idx < 0 || module_idx >= st->module_count)
        return NULL;
    const station_module_t *m = &st->modules[module_idx];
    if (m->scaffold) {
        commodity_t mat = module_build_material_lookup(m->type);
        float cost = module_build_cost_lookup(m->type);
        if (mat < COMMODITY_COUNT) {
            snprintf(label, sizeof(label), "needs %.0f %s to come online",
                     cost, commodity_short_name(mat));
            return label;
        }
    }

    switch (m->type) {
    case MODULE_FURNACE: {
        commodity_t ore = module_instance_input_ore(m);
        commodity_t out = module_instance_output(m);
        if (ore < COMMODITY_COUNT && out < COMMODITY_COUNT) {
            snprintf(label, sizeof(label), "%s -> %s",
                     commodity_name(ore), commodity_name(out));
            return label;
        }
        break;
    }
    case MODULE_FRAME_PRESS:
    case MODULE_LASER_FAB:
    case MODULE_TRACTOR_FAB: {
        module_inputs_t req = module_instance_required_inputs(m);
        commodity_t out = module_instance_output(m);
        if (req.count <= 0 || out >= COMMODITY_COUNT) break;
        char inputs[72] = "";
        for (int i = 0; i < req.count; i++) {
            if (i > 0) hud_append_text(inputs, sizeof(inputs), " + ");
            hud_append_text(inputs, sizeof(inputs),
                            commodity_name(req.commodities[i]));
        }
        snprintf(label, sizeof(label), "%s -> %s",
                 inputs, commodity_name(out));
        return label;
    }
    case MODULE_SHIPYARD:
        snprintf(label, sizeof(label),
                 "Frames + Laser Modules + Tractor Modules -> ships/kits");
        return label;
    case MODULE_HOPPER:
        if ((commodity_t)m->commodity < COMMODITY_COUNT) {
            snprintf(label, sizeof(label), "feeds %s into station work",
                     commodity_name((commodity_t)m->commodity));
            return label;
        }
        break;
    case MODULE_SIGNAL_RELAY:
        return "extends station signal";
    case MODULE_DOCK:
        return "local credits, contracts, refit";
    case MODULE_REPAIR_BAY:
        return "turns repair kits into hull";
    default:
        break;
    }
    return NULL;
}

static const NetPlayerState *hud_net_player_state(int idx) {
    if (!g.net_authority_enabled || idx < 0 || idx >= NET_MAX_PLAYERS)
        return NULL;
    const NetPlayerState *players = net_get_interpolated_players();
    if (players && players[idx].active) return &players[idx];
    players = net_get_players();
    if (players && players[idx].active) return &players[idx];
    return NULL;
}

static void hud_player_scan_label(int idx, char *out, size_t cap) {
    if (!out || cap == 0) return;
    const NetPlayerState *np = hud_net_player_state(idx);
    if (np && np->callsign[0]) {
        snprintf(out, cap, "%s", np->callsign);
    } else {
        snprintf(out, cap, "ID %d", idx);
    }
}

static const char *hud_npc_role_label(uint8_t role);

static const char *hud_npc_action_hint(const npc_ship_t *npc) {
    if (!npc) return "scan";
    switch (npc->role) {
    case NPC_ROLE_MINER:
        if (npc->state == NPC_STATE_MINING ||
            npc->state == NPC_STATE_TRAVEL_TO_ASTEROID) {
            return "compete for rock";
        }
        if (npc->state == NPC_STATE_RETURN_TO_STATION ||
            npc->state == NPC_STATE_DOCKED) {
            return "follow to refinery";
        }
        return "follow ore route";
    case NPC_ROLE_HAULER:
        if (npc->state == NPC_STATE_TRAVEL_TO_DEST ||
            npc->state == NPC_STATE_UNLOADING) {
            return "follow route";
        }
        if (npc->state == NPC_STATE_DOCKED) {
            return "watch for departure";
        }
        return "intercept cargo";
    case NPC_ROLE_TOW:
        return "follow tow path";
    default:
        return "hail or follow";
    }
}

static hud_action_t hud_classify_action(int cargo_units, int cargo_capacity, float sig_quality) {
    hud_action_t out = {0};
    out.kind = HUD_ACTION_IDLE;
    out.grade = (int)MINING_GRADE_COMMON;
    if (LOCAL_PLAYER.docked) { out.kind = HUD_ACTION_DOCKED; return out; }
    /* Target asteroid (laser-pointed). */
    if (LOCAL_PLAYER.hover_asteroid >= 0 &&
        g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        const asteroid_t *a = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
        out.kind = HUD_ACTION_TARGET_ASTEROID;
        out.int_a = (int)lroundf(a->hp);
        out.tier = (int)a->tier;
        out.commodity = (int)a->commodity;
        out.grade = (int)a->grade;
        const char *gate = hud_asteroid_gate_reason(a);
        out.int_b = gate ? 1 : 0;
        if (gate) {
            snprintf(out.reason, sizeof(out.reason), "%s", gate);
        } else {
            hud_usefulness_t usefulness = hud_asteroid_usefulness(a);
            snprintf(out.reason, sizeof(out.reason), "%s", usefulness.label);
            out.reason_has_color = usefulness.has_color;
            memcpy(out.reason_color, usefulness.color,
                   sizeof(out.reason_color));
        }
        return out;
    }
    /* Scan results take precedence over towing/fragments — the player
     * actively pointed the beam at a thing they want info about. */
    if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 1) {
        const station_t *st = &g.world.stations[LOCAL_PLAYER.scan_target_index];
        out.kind = HUD_ACTION_SCAN_MODULE;
        out.str_a = st->name;
        if (LOCAL_PLAYER.scan_module_index >= 0) {
            out.str_b = module_type_name(st->modules[LOCAL_PLAYER.scan_module_index].type);
            out.str_c = hud_module_consequence(st, LOCAL_PLAYER.scan_module_index);
        }
        return out;
    }
    if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 2) {
        const npc_ship_t *npc = &g.world.npc_ships[LOCAL_PLAYER.scan_target_index];
        out.kind = HUD_ACTION_SCAN_NPC;
        out.str_a = hud_npc_role_label(npc->role);
        out.str_b = hud_npc_action_hint(npc);
        int total = 0;
        for (int ci = 0; ci < COMMODITY_COUNT; ci++) total += (int)lroundf(npc->ship->cargo[ci]);
        out.int_a = total;
        return out;
    }
    if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 3) {
        const NetPlayerState *np = hud_net_player_state(LOCAL_PLAYER.scan_target_index);
        out.kind = HUD_ACTION_SCAN_PILOT;
        out.int_a = LOCAL_PLAYER.scan_target_index;
        out.str_a = (np && np->callsign[0]) ? np->callsign : NULL;
        if (LOCAL_PLAYER.scan_target_index >= 0 &&
            LOCAL_PLAYER.scan_target_index < MAX_PLAYERS &&
            g.world.players[LOCAL_PLAYER.scan_target_index].ship->hull > 0.0f)
            out.int_b = (int)lroundf(g.world.players[LOCAL_PLAYER.scan_target_index].ship->hull);
        return out;
    }
    if (LOCAL_PLAYER.scan_active &&
        LOCAL_PLAYER.scan_target_type == INSPECT_TARGET_CARGO_POD) {
        int idx = LOCAL_PLAYER.scan_target_index;
        if (idx >= 0 && idx < MAX_CARGO_PODS) {
            const cargo_pod_t *pod = &g.world.cargo_pods[idx];
            if (pod->active) {
                out.kind = HUD_ACTION_SCAN_CARGO_POD;
                out.commodity = pod->commodity;
                out.grade = (int)cargo_pod_display_grade(pod);
                out.int_a = pod->quantity;
                out.int_b = pod->kind;
                return out;
            }
        }
    }
    if (mining_client_get()->fracture_search_timer > 0.0f) {
        out.kind = HUD_ACTION_MINING;
        return out;
    }
    int towed_fragments = ship_towed_fragment_count(LOCAL_PLAYER.ship);
    int towed_pods = ship_towed_pod_count(LOCAL_PLAYER.ship);
    if (towed_fragments > 0 || towed_pods > 0) {
        out.kind = HUD_ACTION_TOWING;
        out.int_a = towed_fragments + towed_pods;
        out.int_b = LOCAL_PLAYER.ship->tractor_active ? 1 : 0;
        out.tier = towed_fragments;
        out.commodity = towed_pods;
        out.grade = (int)hud_best_towed_fragment_grade();
        hud_usefulness_t best = {0};
        for (int t = 0; t < LOCAL_PLAYER.ship->towed_count; t++) {
            int idx = LOCAL_PLAYER.ship->towed_fragments[t];
            if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
            const asteroid_t *a = &g.world.asteroids[idx];
            hud_usefulness_t usefulness = hud_asteroid_usefulness(a);
            if (rock_usefulness_is_stronger(&usefulness.candidate,
                                            &best.candidate))
                best = usefulness;
        }
        if (best.label[0]) {
            snprintf(out.reason, sizeof(out.reason), "%s", best.label);
            out.reason_has_color = best.has_color;
            memcpy(out.reason_color, best.color, sizeof(out.reason_color));
        }
        return out;
    }
    if (LOCAL_PLAYER.nearby_fragments > 0) {
        out.grade = (int)hud_best_nearby_fragment_grade();
        if (LOCAL_PLAYER.ship->tractor_active && LOCAL_PLAYER.tractor_fragments > 0) {
            out.kind = HUD_ACTION_TRACTOR_LOCK;
            out.int_a = LOCAL_PLAYER.tractor_fragments;
            out.int_b = LOCAL_PLAYER.nearby_fragments;
            return out;
        }
        if (LOCAL_PLAYER.ship->tractor_active) {
            out.kind = HUD_ACTION_TRACTOR_REACHING;
            out.int_b = LOCAL_PLAYER.nearby_fragments;
            return out;
        }
        out.kind = HUD_ACTION_FRAGMENTS_NEARBY;
        out.int_b = LOCAL_PLAYER.nearby_fragments;
        return out;
    }
    if (cargo_units >= cargo_capacity) {
        out.kind = HUD_ACTION_HOLD_FULL;
        return out;
    }
    /* Station-local ledger balances are passive: docking makes the balance
     * usable automatically, while H remains a hail/contact action. Surface the
     * largest off-ship balance without implying a separate collect button. */
    if (sig_quality >= 0.90f) {
        int best_idx = -1, others = 0;
        float best_bal = 0.0f;
        if (client_ledger_balance_summary(&best_idx, &best_bal, &others)) {
            out.kind = HUD_ACTION_LEDGER_BALANCE;
            out.int_a = (int)lroundf(best_bal);
            out.int_b = others;
            const char *cn = g.world.stations[best_idx].currency_name;
            out.str_a = (cn && cn[0]) ? cn : "credits";
            return out;
        }
    }
    return out;
}

static void hud_format_action_compact(const hud_action_t *a, const char *dock_role,
                                      char *out, size_t out_size) {
    switch (a->kind) {
    case HUD_ACTION_DOCKED:
        snprintf(out, out_size, "%s CONSOLE", dock_role);
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        if (a->reason[0]) {
            snprintf(out, out_size, "TGT %s // %s // %s",
                     asteroid_tier_name((asteroid_tier_t)a->tier),
                     commodity_code((commodity_t)a->commodity),
                     a->reason);
        } else {
            snprintf(out, out_size, "TGT %s // %s // %d HP",
                     asteroid_tier_name((asteroid_tier_t)a->tier),
                     commodity_code((commodity_t)a->commodity),
                     a->int_a);
        }
        return;
    case HUD_ACTION_SCAN_MODULE:
        if (a->str_b && a->str_c)
            snprintf(out, out_size, "SCAN %s // %s // %s",
                     a->str_a, a->str_b, a->str_c);
        else if (a->str_b)
            snprintf(out, out_size, "SCAN %s // %s", a->str_a, a->str_b);
        else
            snprintf(out, out_size, "SCAN %s // CORE", a->str_a);
        return;
    case HUD_ACTION_SCAN_NPC:
        if (a->str_b)
            snprintf(out, out_size, "SCAN NPC // %s", a->str_b);
        else
            snprintf(out, out_size, "SCAN NPC // %s",
                     (a->str_a && a->str_a[0] == 'm') ? "MINER" : "HAULER");
        return;
    case HUD_ACTION_SCAN_PILOT:
        if (a->str_a) snprintf(out, out_size, "SCAN PILOT // %s", a->str_a);
        else          snprintf(out, out_size, "SCAN PILOT // ID %d", a->int_a);
        return;
    case HUD_ACTION_SCAN_CARGO_POD:
        if (a->commodity >= 0 && a->commodity < COMMODITY_COUNT) {
            snprintf(out, out_size, "SCAN %s // %s x%d // %s",
                     a->int_b == CARGO_POD_GAS ? "GAS" : "CRATE",
                     commodity_code((commodity_t)a->commodity),
                     a->int_a,
                     mining_grade_label((mining_grade_t)a->grade));
        } else {
            snprintf(out, out_size, "SCAN %s // x%d",
                     a->int_b == CARGO_POD_GAS ? "GAS" : "CRATE",
                     a->int_a);
        }
        return;
    case HUD_ACTION_MINING:
        snprintf(out, out_size, "MINING... // CLAIM WINDOW");
        return;
    case HUD_ACTION_TOWING:
        if (a->reason[0] && a->commodity == 0)
            snprintf(out, out_size, "TOWING %d // %s", a->int_a, a->reason);
        else if (a->int_b)
            snprintf(out, out_size, "TOWING %d // TRACTOR", a->int_a);
        else if (a->commodity > 0 && a->tier == 0)
            snprintf(out, out_size, "TOWING %d CRATE%s // tap [Space] drop",
                     a->commodity, a->commodity == 1 ? "" : "S");
        else
            snprintf(out, out_size, "TOWING %d // tap [Space] release", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_LOCK:
        snprintf(out, out_size, "TRACTOR // %d FRAG", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_REACHING:
        snprintf(out, out_size, "hold [Space] TRACTOR // %d", a->int_b);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        snprintf(out, out_size, "hold [Space] TRACTOR // %d nearby", a->int_b);
        return;
    case HUD_ACTION_HOLD_FULL:
        snprintf(out, out_size, "Hold full. Dock to sell.");
        return;
    case HUD_ACTION_LEDGER_BALANCE:
        if (a->int_b > 0) {
            snprintf(out, out_size, "%d %s (+%d) // dock",
                     a->int_a, a->str_a ? a->str_a : "credits", a->int_b);
        } else {
            snprintf(out, out_size, "%d %s // dock",
                     a->int_a, a->str_a ? a->str_a : "credits");
        }
        return;
    case HUD_ACTION_IDLE:
    default:
        snprintf(out, out_size, "Nothing in range. Scan for rocks.");
        return;
    }
}

static void hud_format_action_wide(const hud_action_t *a, const station_t *current_station,
                                   char *out, size_t out_size) {
    switch (a->kind) {
    case HUD_ACTION_DOCKED:
        snprintf(out, out_size, "%s console",
                 current_station ? station_role_name(current_station) : "Station");
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        if (a->reason[0]) {
            snprintf(out, out_size, "Target %s // %s // %s",
                     asteroid_tier_kind((asteroid_tier_t)a->tier),
                     commodity_short_name((commodity_t)a->commodity),
                     a->reason);
        } else {
            snprintf(out, out_size, "Target %s // %s // %d hp",
                     asteroid_tier_kind((asteroid_tier_t)a->tier),
                     commodity_short_name((commodity_t)a->commodity),
                     a->int_a);
        }
        return;
    case HUD_ACTION_SCAN_MODULE:
        if (a->str_b && a->str_c)
            snprintf(out, out_size, "Scan %s // %s // %s",
                     a->str_a, a->str_b, a->str_c);
        else if (a->str_b)
            snprintf(out, out_size, "Scan %s // %s", a->str_a, a->str_b);
        else
            snprintf(out, out_size, "Scan %s // core hub", a->str_a);
        return;
    case HUD_ACTION_SCAN_NPC:
        if (a->str_b)
            snprintf(out, out_size, "Scan NPC %s // %s // cargo %d",
                     a->str_a, a->str_b, a->int_a);
        else
            snprintf(out, out_size, "Scan NPC %s // cargo %d",
                     a->str_a, a->int_a);
        return;
    case HUD_ACTION_SCAN_PILOT:
        if (a->str_a && a->int_b > 0)
            snprintf(out, out_size, "Scan pilot %s // hull %d", a->str_a, a->int_b);
        else if (a->str_a)
            snprintf(out, out_size, "Scan pilot %s", a->str_a);
        else
            snprintf(out, out_size, "Scan pilot %d", a->int_a);
        return;
    case HUD_ACTION_SCAN_CARGO_POD:
        if (a->commodity >= 0 && a->commodity < COMMODITY_COUNT) {
            snprintf(out, out_size, "Scan %s // %s x%d // %s grade",
                     a->int_b == CARGO_POD_GAS ? "gas" : "crate",
                     commodity_short_name((commodity_t)a->commodity),
                     a->int_a,
                     mining_grade_label((mining_grade_t)a->grade));
        } else {
            snprintf(out, out_size, "Scan %s // x%d",
                     a->int_b == CARGO_POD_GAS ? "gas" : "crate",
                     a->int_a);
        }
        return;
    case HUD_ACTION_MINING:
        snprintf(out, out_size, "Mining... // claim window");
        return;
    case HUD_ACTION_TOWING:
        if (a->reason[0] && a->commodity == 0)
            snprintf(out, out_size, "Towing %d // %s", a->int_a, a->reason);
        else if (a->int_b)
            snprintf(out, out_size, "Towing %d // tractor on", a->int_a);
        else if (a->commodity > 0 && a->tier == 0)
            snprintf(out, out_size, "Towing %d crate%s // tap [Space] to drop",
                     a->commodity, a->commodity == 1 ? "" : "s");
        else
            snprintf(out, out_size, "Towing %d // tap [Space] to release", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_LOCK:
        snprintf(out, out_size, "Tractor lock // %d frag%s",
                 a->int_a, a->int_a == 1 ? "" : "s");
        return;
    case HUD_ACTION_TRACTOR_REACHING:
        snprintf(out, out_size, "Tractor reaching // %d nearby", a->int_b);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        snprintf(out, out_size, "Hold [Space] tractor // %d nearby", a->int_b);
        return;
    case HUD_ACTION_HOLD_FULL:
        snprintf(out, out_size, "Hold full. Dock to sell.");
        return;
    case HUD_ACTION_LEDGER_BALANCE:
        if (a->int_b > 0) {
            snprintf(out, out_size, "%d %s available // dock to spend (+%d more)",
                     a->int_a, a->str_a ? a->str_a : "credits", a->int_b);
        } else {
            snprintf(out, out_size, "%d %s available // dock to spend",
                     a->int_a, a->str_a ? a->str_a : "credits");
        }
        return;
    case HUD_ACTION_IDLE:
    default:
        snprintf(out, out_size, "No target // line up a rock");
        return;
    }
}

static void hud_set_action_color(const hud_action_t *a) {
    if (a->reason_has_color) {
        sdtx_color3b(a->reason_color[0], a->reason_color[1],
                     a->reason_color[2]);
        return;
    }
    switch (a->kind) {
    case HUD_ACTION_SCAN_MODULE:
    case HUD_ACTION_SCAN_NPC:
    case HUD_ACTION_SCAN_PILOT:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        return;
    case HUD_ACTION_SCAN_CARGO_POD:
        hud_set_grade_color((uint8_t)a->grade);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        hud_set_grade_color((uint8_t)a->grade);
        return;
    case HUD_ACTION_HOLD_FULL:
    case HUD_ACTION_LEDGER_BALANCE:
        sdtx_color3b(PAL_ORE_AMBER);
        return;
    case HUD_ACTION_IDLE:
        sdtx_color3b(PAL_TEXT_MUTED);
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        if (a->int_b) {
            sdtx_color3b(PAL_WARNING);
            return;
        }
        if (a->tier == (int)ASTEROID_TIER_S) {
            hud_set_grade_color((uint8_t)a->grade);
            return;
        }
        sdtx_color3b(PAL_ACTIVE);
        return;
    case HUD_ACTION_TOWING:
    case HUD_ACTION_TRACTOR_LOCK:
    case HUD_ACTION_TRACTOR_REACHING:
        hud_set_grade_color((uint8_t)a->grade);
        return;
    case HUD_ACTION_DOCKED:
    case HUD_ACTION_MINING:
    default:
        sdtx_color3b(PAL_ACTIVE);
        return;
    }
}

/* Compact renderer — short-form ALL CAPS at the bottom of the top panel. */
static void hud_render_action_compact(const hud_action_t *a, const char *dock_role) {
    char text[128];
    hud_format_action_compact(a, dock_role, text, sizeof(text));
    hud_set_action_color(a);
    sdtx_puts(text);
}

/* Wide renderer — full English at the bottom of the top panel. */
static void hud_render_action_wide(const hud_action_t *a, const station_t *current_station) {
    char text[160];
    hud_format_action_wide(a, current_station, text, sizeof(text));
    hud_set_action_color(a);
    sdtx_puts(text);
}

static void hud_format_region_label(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
#ifdef __EMSCRIPTEN__
    (void)signal_relay_debug_region_js(out, (int)cap);
#endif
    if (out[0] == '\0') {
        if (g.net_authority_enabled)
            snprintf(out, cap, "direct");
        else
            snprintf(out, cap, "solo");
    }
}

static void hud_format_latency_label(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    float ping_ms =
        net_latency_stats_fresh(&g.net_ping_latency,
                                g.net_time,
                                NET_LATENCY_STALE_SEC)
        ? net_latency_stats_smoothed_sec(&g.net_ping_latency) * 1000.0f
        : 0.0f;
    float ack_ms =
        net_latency_stats_fresh(&g.net_ack_latency,
                                g.net_time,
                                NET_LATENCY_STALE_SEC)
        ? net_latency_stats_smoothed_sec(&g.net_ack_latency) * 1000.0f
        : 0.0f;
    if (ping_ms > 0.0f && ack_ms > 0.0f) {
        snprintf(out, cap, "ping %.0fms ack %.0fms", ping_ms, ack_ms);
        return;
    }
    if (ping_ms > 0.0f) {
        snprintf(out, cap, "ping %.0fms", ping_ms);
        return;
    }
    if (ack_ms > 0.0f) {
        snprintf(out, cap, "ack %.0fms", ack_ms);
        return;
    }

#ifdef __EMSCRIPTEN__
    int health_ms = signal_relay_debug_health_latency_ms_js();
    int broker_ms = signal_relay_debug_broker_latency_ms_js();
    if (health_ms > 0 && broker_ms > 0)
        snprintf(out, cap, "boot %dms health %dms", broker_ms, health_ms);
    else if (health_ms > 0)
        snprintf(out, cap, "health %dms", health_ms);
    else if (broker_ms > 0)
        snprintf(out, cap, "boot %dms", broker_ms);
    else
#endif
        snprintf(out, cap, "latency pending");
}

/* ------------------------------------------------------------------ */
/* Shared post-classify panels — render in BOTH compact and wide so a  */
/* small window doesn't silently lose signal-lost warnings, net status, */
/* hail sigil, etc. Each is a one-shot draw guarded by its own state. */
/* ------------------------------------------------------------------ */

static void hud_draw_alpha_banner_and_connection(float screen_w, bool compact) {
    /* Version / connection status — top right */
    float info_x = ui_text_pos(fmaxf(8.0f, screen_w - (compact ? 100.0f : 140.0f)));
    float info_y = ui_text_pos(8.0f);
    sdtx_pos(info_x, info_y);
#ifdef GIT_HASH
    const char *client_hash = GIT_HASH;
#else
    const char *client_hash = "dev";
#endif
    if (g.net_authority_enabled && net_is_connected()) {
        const char *srv = net_server_hash();
        bool match = srv[0] != '\0' && strcmp(client_hash, srv) == 0;
        if (net_is_loopback())  { sdtx_color3b(PAL_SYNC_OK);          sdtx_puts("local"); }
        else if (match)         { sdtx_color3b(PAL_SYNC_OK);          sdtx_printf("v%s", client_hash); }
        else if (srv[0] == '\0'){ sdtx_color3b(PAL_SYNC_CONNECTING);  sdtx_puts("connecting..."); }
        else                    { sdtx_color3b(PAL_SYNC_RESYNCING);   sdtx_puts("syncing..."); }
        bool ping_fresh = net_latency_stats_fresh(&g.net_ping_latency,
                                                  g.net_time,
                                                  NET_LATENCY_STALE_SEC);
        bool ack_fresh = net_latency_stats_fresh(&g.net_ack_latency,
                                                 g.net_time,
                                                 NET_LATENCY_STALE_SEC);
        if (ack_fresh || ping_fresh) {
            float net_x = ui_text_pos(fmaxf(8.0f, screen_w - (compact ? 176.0f : 304.0f)));
            float ack_ms = ack_fresh
                ? net_latency_stats_smoothed_sec(&g.net_ack_latency) * 1000.0f
                : 0.0f;
            float ping_ms = ping_fresh
                ? net_latency_stats_smoothed_sec(&g.net_ping_latency) * 1000.0f
                : 0.0f;
            float gap_ms = net_latency_gap_stats_fresh(
                &g.net_ack_gap, g.net_time, NET_LATENCY_STALE_SEC)
                ? net_latency_gap_stats_smoothed_sec(&g.net_ack_gap) * 1000.0f
                : 0.0f;
            float warning_ms = (ack_ms > 0.0f) ? ack_ms : ping_ms;
            sdtx_pos(net_x, info_y + 1.2f);
            if (warning_ms >= HUD_LATENCY_BAD_MS)
                sdtx_color3b(PAL_WARNING);
            else if (warning_ms >= HUD_LATENCY_WARN_MS)
                sdtx_color3b(PAL_READY_YELLOW);
            else
                sdtx_color3b(PAL_TEXT_GREY);
            if (compact) {
                if (ping_ms > 0.0f && ack_ms > 0.0f)
                    sdtx_printf("ping %.0fms ack %.0fms", ping_ms, ack_ms);
                else if (ping_ms > 0.0f)
                    sdtx_printf("ping %.0fms", ping_ms);
                else
                    sdtx_printf("ack %.0fms", ack_ms);
                if (ping_ms > 0.0f && ack_ms > 0.0f) {
                    sdtx_pos(net_x, info_y + 2.4f);
                    if (g.net_replay_count > 0) {
                        sdtx_printf("excess %.0fms history %u",
                                    gap_ms,
                                    (unsigned)g.net_replay_count);
                    } else {
                        sdtx_printf("excess %.0fms queue %u",
                                    gap_ms,
                                    (unsigned)g.net_action_queue_count);
                    }
                }
            } else if (ping_ms > 0.0f && ack_ms > 0.0f) {
                sdtx_printf("ping %.0fms ack %.0fms excess %.0fms queue %u history %u",
                            ping_ms,
                            ack_ms,
                            gap_ms,
                            (unsigned)g.net_action_queue_count,
                            (unsigned)g.net_replay_count);
            } else if (ping_ms > 0.0f) {
                sdtx_printf("ping %.0fms queue %u replay %u",
                            ping_ms,
                            (unsigned)g.net_action_queue_count,
                            (unsigned)g.net_replay_count);
            } else {
                sdtx_printf("ack %.0fms queue %u replay %u",
                            ack_ms,
                            (unsigned)g.net_action_queue_count,
                            (unsigned)g.net_replay_count);
            }
        }
    } else if (g.net_authority_enabled) {
        sdtx_color3b(PAL_SYNC_OFFLINE);
        sdtx_puts("offline [P] reconnect");
    } else {
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_printf("v%s", client_hash);
    }
    /* Player callsign is rendered at top_row_0 below (see the "Use
     * the SESSION callsign" block) — derived from the pubkey via
     * mining_alphanumeric_callsign() once #513 lands. Don't double up
     * with a separate identity line here. */

    /* Alpha banner: repeating ticker across the top. */
    float bw = ui_safe_positive(sapp_widthf(), 1280.0f) /
               fmaxf(1.0f, ui_safe_positive(sapp_dpi_scale(), 1.0f));
    int cols = (int)(bw / 8.0f);
    char banner[512];
    char segment[192];
    char region[48];
    char latency[80];
    hud_format_region_label(region, sizeof(region));
    hud_format_latency_label(latency, sizeof(latency));
    if (g.net_authority_enabled) {
        snprintf(segment, sizeof(segment),
                 "ALPHA // v%s // %s // %s // preview build // ",
                 client_hash, region, latency);
    } else {
        snprintf(segment, sizeof(segment),
                 "ALPHA // v%s // solo // %s // ",
                 client_hash, latency);
    }
    int seg_len = (int)strlen(segment);
    int pos = 0;
    while (pos < cols && pos < (int)sizeof(banner) - 1) {
        int left = (int)sizeof(banner) - 1 - pos;
        int copy = seg_len < left ? seg_len : left;
        memcpy(&banner[pos], segment, copy);
        pos += copy;
    }
    banner[pos < (int)sizeof(banner) ? pos : (int)sizeof(banner) - 1] = '\0';
    sdtx_pos(0.0f, 0.0f);
    sdtx_color3b(PAL_ALPHA_BANNER);
    sdtx_puts(banner);
}

/* Nearest station name in the bottom-left when undocked, plus the
 * housekeeping that expires a tracked contract once the server retired
 * it. The contract cleanup runs every frame regardless of layout. */
static void hud_draw_nav_label(float screen_w, float screen_h) {
    if (LOCAL_PLAYER.docked) return;
    const station_t *nav_st = navigation_station_ptr();
    if (nav_st && nav_st->name[0] != '\0') {
        sdtx_pos(ui_text_pos(16.0f), ui_text_pos(screen_h - 20.0f));
        sdtx_color3b(PAL_TEXT_FADED);
        int name_cols = (int)((screen_w - 24.0f) / 8.0f);
        sdtx_printf("%.*s", name_cols, nav_st->name);
    }
    if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
        /* Drop tracking when the contract closes OR when it leaves the
         * player's gossip mask (e.g., overwritten by FIFO eviction at a
         * later dock). */
        if (!g.world.contracts[g.tracked_contract].active ||
            !(g.player_known_contract_mask & (1u << g.tracked_contract)))
            g.tracked_contract = -1;
    }
}

/* Station hail sigil — mini profile pic in the lower-left that fades
 * in with hail_timer. "Caller ID" for the hint-bar message above. */
static void hud_draw_hail_sigil(float screen_w, float screen_h) {
    if (g.hail_timer <= 0.0f) return;
    if (g.hail_station_index < 0 || g.hail_station_index >= MAX_STATIONS) return;
    const avatar_cache_t *av = avatar_get(g.hail_station_index);
    if (!av || !av->texture_valid) return;

    float alpha = fminf(g.hail_timer / 0.5f, 1.0f);
    float sig_size = 48.0f;
    float sx0 = 16.0f;
    float sy0 = screen_h - sig_size - 32.0f;
    render_set_screen_space(screen_w, screen_h);
    draw_texture_rect(av->view_id, av->sampler_id,
                      sx0, sy0, sx0 + sig_size, sy0 + sig_size,
                      alpha, alpha, alpha, alpha);
    /* Gold border frame — "transmission active" radio indicator. */
    float border_a = 0.70f * alpha;
    sgl_begin_lines();
    sgl_c4f(0.78f, 0.63f, 0.19f, border_a);
    sgl_v2f(sx0,            sy0);            sgl_v2f(sx0 + sig_size, sy0);
    sgl_v2f(sx0 + sig_size, sy0);            sgl_v2f(sx0 + sig_size, sy0 + sig_size);
    sgl_v2f(sx0 + sig_size, sy0 + sig_size); sgl_v2f(sx0,            sy0 + sig_size);
    sgl_v2f(sx0,            sy0 + sig_size); sgl_v2f(sx0,            sy0);
    sgl_end();
}

static void hud_draw_module_inspect_pane(float screen_w) {
    if (g.inspect_station < 0 || g.inspect_station >= MAX_STATIONS) return;
    if (g.inspect_module < 0) return;
    if (LOCAL_PLAYER.docked) return;
    const station_t *ist = &g.world.stations[g.inspect_station];
    if (!station_exists(ist) || g.inspect_module >= ist->module_count) {
        g.inspect_station = -1;
        return;
    }
    const station_module_t *im = &ist->modules[g.inspect_module];
    station_flow_diag_t flow = station_module_flow_diag_view(
        ist, g.inspect_module, g.net_authority_enabled && net_is_connected());
    float px = fmaxf(16.0f, screen_w - 260.0f);
    float py = 60.0f;
    float cell = 8.0f;
    float next_y = 60.0f;

    sdtx_pos(px / cell, py / cell);
    sdtx_color3b(PAL_ORE_AMBER);
    sdtx_printf("[ %s ]", module_type_name(im->type));

    sdtx_pos(px / cell, (py + 14.0f) / cell);
    sdtx_color3b(PAL_INSPECT_STATION);
    sdtx_printf("Station: %s", ist->name);

    sdtx_pos(px / cell, (py + 28.0f) / cell);
    sdtx_color3b(PAL_INSPECT_LOCATION);
    sdtx_printf("Ring %d  Slot %d", im->ring, im->slot);

    if (im->scaffold) {
        sdtx_pos(px / cell, (py + 42.0f) / cell);
        if (im->build_progress < 1.0f) {
            int pct = (int)lroundf(im->build_progress * 100.0f);
            sdtx_color3b(PAL_BUILD_SUPPLYING);
            sdtx_printf("SUPPLYING: %d%%", pct);
        } else {
            int pct = (int)lroundf((im->build_progress - 1.0f) * 100.0f);
            sdtx_color3b(PAL_BUILD_BUILDING);
            sdtx_printf("BUILDING: %d%%", pct);
        }
    } else {
        sdtx_pos(px / cell, (py + 42.0f) / cell);
        sdtx_color3b(PAL_ACTIVE);
        sdtx_puts("ONLINE");
    }
    if (flow != STATION_FLOW_DIAG_NONE) {
        sdtx_pos(px / cell, (py + next_y) / cell);
        if (flow == STATION_FLOW_DIAG_RUNNING)
            sdtx_color3b(120, 230, 180);
        else if (flow == STATION_FLOW_DIAG_SLOW_FEED)
            sdtx_color3b(245, 210, 115);
        else
            sdtx_color3b(255, 135, 120);
        sdtx_printf("FLOW: %s", station_flow_diag_label(flow));
        next_y += 14.0f;
    }
    sdtx_pos(px / cell, (py + next_y) / cell);
    sdtx_color3b(PAL_TEXT_GREY);
    sdtx_puts("[E] close");
}

static const char *hud_npc_role_label(uint8_t role) {
    switch ((npc_role_t)role) {
    case NPC_ROLE_MINER:  return "miner";
    case NPC_ROLE_HAULER: return "hauler";
    case NPC_ROLE_TOW:    return "tow";
    default: return "npc";
    }
}

static const char *hud_npc_state_label(uint8_t state) {
    switch ((npc_state_t)state) {
    case NPC_STATE_IDLE:               return "idle";
    case NPC_STATE_TRAVEL_TO_ASTEROID: return "to rock";
    case NPC_STATE_MINING:             return "mining";
    case NPC_STATE_RETURN_TO_STATION:  return "return";
    case NPC_STATE_DOCKED:             return "docked";
    case NPC_STATE_TRAVEL_TO_DEST:     return "to dest";
    case NPC_STATE_UNLOADING:          return "unload";
    default: return "unknown";
    }
}

static const char *hud_hull_class_label(uint8_t hull_class) {
    if (hull_class < (uint8_t)HULL_CLASS_COUNT)
        return HULL_DEFS[hull_class].name;
    return "ship";
}

static const char *hud_grade_short_label(uint8_t grade) {
    if (grade == (uint8_t)MINING_GRADE_COMMISSIONED) return "comm";
    return mining_grade_label((mining_grade_t)grade);
}

static bool hash32_is_zero(const uint8_t hash[32]);
static void hud_hash_detail_label(const uint8_t hash[32],
                                  char *out,
                                  size_t cap);

static const char *hud_inspect_diag_label(uint8_t kind) {
    switch ((inspect_diag_kind_t)kind) {
    case INSPECT_DIAG_MARKET_DEMAND:      return "memory demand";
    case INSPECT_DIAG_MARKET_SUPPLY:      return "memory supply";
    case INSPECT_DIAG_ROUTE_DANGER:       return "route danger";
    case INSPECT_DIAG_ROUTE_SUCCESS:      return "route success";
    case INSPECT_DIAG_DELIVERY_RECEIPT:   return "delivery proof";
    case INSPECT_DIAG_RECEIPT_LINK:       return "receipt link";
    case INSPECT_DIAG_ROUTE_REPUTATION:   return "route trust";
    case INSPECT_DIAG_ROUTE_RISK:         return "route risk";
    case INSPECT_DIAG_STATION_TRUST:      return "station trust";
    case INSPECT_DIAG_STATION_RISK:       return "station risk";
    case INSPECT_DIAG_JOB_MINE:           return "job mine";
    case INSPECT_DIAG_JOB_HAUL:           return "job haul";
    case INSPECT_DIAG_JOB_TOW:            return "job tow";
    case INSPECT_DIAG_JOB_DELIVER_PROOF:  return "job proof";
    case INSPECT_DIAG_JOB_SCOUT:          return "job scout";
    case INSPECT_DIAG_JOB_REPAIR:         return "job repair";
    case INSPECT_DIAG_HNN_TRACE:          return "memory trace";
    case INSPECT_DIAG_NONE:
    default:                              return "memory";
    }
}

static bool hud_inspect_diag_is_job(uint8_t kind) {
    switch ((inspect_diag_kind_t)kind) {
    case INSPECT_DIAG_JOB_MINE:
    case INSPECT_DIAG_JOB_HAUL:
    case INSPECT_DIAG_JOB_TOW:
    case INSPECT_DIAG_JOB_DELIVER_PROOF:
    case INSPECT_DIAG_JOB_SCOUT:
    case INSPECT_DIAG_JOB_REPAIR:
        return true;
    default:
        return false;
    }
}

static const char *hud_job_factor_name(int idx) {
    switch (idx) {
    case INSPECT_JOB_FACTOR_VALUE:      return "payout";
    case INSPECT_JOB_FACTOR_DEMAND:     return "demand";
    case INSPECT_JOB_FACTOR_SUPPLY:     return "supply";
    case INSPECT_JOB_FACTOR_ROUTE:      return "route";
    case INSPECT_JOB_FACTOR_FRESHNESS:  return "fresh";
    case INSPECT_JOB_FACTOR_CAPABILITY: return "hull";
    case INSPECT_JOB_FACTOR_PROOF:      return "proof";
    case INSPECT_JOB_FACTOR_HOLOGRAM:   return "hologram";
    default:                            return "signal";
    }
}

static const char *hud_job_base_reason(uint8_t reason) {
    switch ((inspect_job_reason_t)reason) {
    case INSPECT_JOB_REASON_LOCAL_CONTRACT:     return "known contract";
    case INSPECT_JOB_REASON_MARKET_DEMAND:      return "heard demand";
    case INSPECT_JOB_REASON_REMOTE_SUPPLY:      return "remote supply";
    case INSPECT_JOB_REASON_RECEIPT_PROOF:      return "receipt proof";
    case INSPECT_JOB_REASON_STATION_TRUST:      return "station trust";
    case INSPECT_JOB_REASON_STATION_RISK:       return "risk adjusted";
    case INSPECT_JOB_REASON_HNN_RESONANCE:      return "hologram match";
    case INSPECT_JOB_REASON_ORE_PRESSURE:       return "ore pressure";
    case INSPECT_JOB_REASON_CONSTRUCTION_PLAN:  return "build plan";
    case INSPECT_JOB_REASON_DELIVERY_PROOF:     return "delivery proof";
    case INSPECT_JOB_REASON_DISTRESS_SIGNAL:    return "distress signal";
    case INSPECT_JOB_REASON_REPAIR_NEED:        return "repair need";
    case INSPECT_JOB_REASON_ROUTE_MEMORY:       return "route memory";
    case INSPECT_JOB_REASON_ROUTE_RISK:         return "route risk";
    case INSPECT_JOB_REASON_GOSSIP_COURIER:     return "gossip courier";
    case INSPECT_JOB_REASON_NONE:
    default:                                    return NULL;
    }
}

static void hud_job_proof_prefix(const NetInspectSnapshotRow *row,
                                 char *out,
                                 size_t cap) {
    inspect_label_job_proof_prefix(row, out, cap);
}

static bool hud_job_source_chain_label(const NetInspectSnapshotRow *row,
                                       char *out,
                                       size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!row) return false;

    uint8_t memory_kind = row->cargo_pub[INSPECT_JOB_META_MEMORY_KIND];
    uint8_t station = row->cargo_pub[INSPECT_JOB_META_SOURCE_STATION];
    uint8_t hops = row->cargo_pub[INSPECT_JOB_META_HOPS];
    uint8_t age = row->cargo_pub[INSPECT_JOB_META_AGE];
    uint8_t proof_kind = row->cargo_pub[INSPECT_JOB_META_PROOF_KIND];
    if (memory_kind == (uint8_t)MARKET_MEMORY_NONE &&
        proof_kind == (uint8_t)INSPECT_JOB_PROOF_NONE) {
        return false;
    }

    const char *station_name = (station < MAX_STATIONS)
        ? g.world.stations[station].name : "?";
    (void)memory_kind;
    (void)hops;
    (void)age;
    (void)proof_kind;
    return inspect_label_job_source_chain(row, station_name, out, cap);
}

static bool hud_market_source_chain_label(const NetInspectSnapshotRow *row,
                                          char *out,
                                          size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!row) return false;

    return inspect_label_market_source_chain(row, out, cap);
}

static const char *hud_cargo_trust_label(
    const NetInspectSnapshotRow *row) {
    if (!row || !row->trust_evaluated) return "";
    if (row->trust_accepted) {
        switch ((cargo_receipt_trust_status_t)row->trust_status) {
        case CARGO_RECEIPT_TRUST_VALID_TRUSTED:
            return "trusted";
        case CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED:
            return "trusted/rotated";
        case CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY:
            return "accepted/unknown";
        case CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY:
            return "accepted/untrusted";
        default:
            return "accepted";
        }
    }
    switch ((cargo_receipt_trust_status_t)row->trust_status) {
    case CARGO_RECEIPT_TRUST_REJECT_CHAIN:
        return "rejected/chain";
    case CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN:
        return "rejected/no-origin";
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE:
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO:
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN:
        return "rejected/origin";
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY:
        return "rejected/authority";
    case CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY:
        return "rejected/unknown";
    case CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY:
        return "rejected/untrusted";
    case CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY:
        return "rejected/revoked";
    case CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS:
    default:
        return "rejected";
    }
}

static void hud_job_reason_label(const NetInspectSnapshotRow *row,
                                 char *out,
                                 size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;

    const char *base = hud_job_base_reason(row->cargo_pub[INSPECT_JOB_META_REASON]);
    if (base) {
        char proof[12];
        hud_job_proof_prefix(row, proof, sizeof(proof));
        snprintf(out, cap, "why %s%s%s", base,
                 proof[0] ? " " : "", proof);
        return;
    }

    int best_a = -1;
    int best_b = -1;
    uint8_t score_a = 0;
    uint8_t score_b = 0;
    for (int i = 0; i < INSPECT_JOB_FACTOR_COUNT; i++) {
        uint8_t score = row->cargo_pub[i];
        if (score > score_a) {
            best_b = best_a;
            score_b = score_a;
            best_a = i;
            score_a = score;
        } else if (score > score_b) {
            best_b = i;
            score_b = score;
        }
    }

    if (best_a < 0 || score_a == 0) {
        snprintf(out, cap, "why pending evidence");
    } else if (best_b >= 0 && score_b > 32) {
        snprintf(out, cap, "why %s + %s",
                 hud_job_factor_name(best_a),
                 hud_job_factor_name(best_b));
    } else {
        snprintf(out, cap, "why %s", hud_job_factor_name(best_a));
    }
}

static ui_clarity_t hud_job_clarity(const NetInspectSnapshotRow *row) {
    const uint8_t HI[3] = { PAL_CONTRACT_READY };
    const uint8_t LO[3] = { PAL_TEXT_FADED };
    if (!row) return ui_clarity_from_evidence(210, 190, 0, HI, LO);
    uint8_t best = row->cargo_pub[0];
    for (int i = 1; i < INSPECT_JOB_FACTOR_COUNT; i++) {
        if (row->cargo_pub[i] > best) best = row->cargo_pub[i];
    }
    uint8_t confidence = row->grade ? row->grade : best;
    uint8_t salience = best ? best : 160;
    uint8_t hops = row->cargo_pub[INSPECT_JOB_META_HOPS];
    return ui_clarity_from_evidence(confidence, salience, hops, HI, LO);
}

static const NetInspectSnapshotRow *hud_inspect_find_hnn_trace(
    const NetInspectSnapshot *snap) {
    if (!snap) return NULL;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        if ((row->flags & INSPECT_ROW_DIAGNOSTIC) &&
            row->commodity == (uint8_t)INSPECT_DIAG_HNN_TRACE) {
            return row;
        }
    }
    return NULL;
}

static float hud_hnn_trace_unit(uint8_t value) {
    return (float)value / 255.0f;
}

static float hud_hnn_trace_margin(const NetInspectSnapshotRow *row) {
    if (!row) return 0.0f;
    return hud_hnn_trace_unit(row->cargo_pub[INSPECT_HNN_TRACE_MARGIN]) *
           2.0f - 1.0f;
}

static float hud_hnn_trace_snr(const NetInspectSnapshotRow *row) {
    if (!row) return 0.0f;
    return hud_hnn_trace_unit(row->cargo_pub[INSPECT_HNN_TRACE_SNR]) * 8.0f;
}

static uint16_t hud_read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t hud_read_u64_le(const uint8_t *p) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--)
        value = (value << 8) | (uint64_t)p[i];
    return value;
}

static ui_clarity_t hud_hnn_trace_clarity(const NetInspectSnapshotRow *row) {
    const uint8_t HI[3] = { PAL_CONTRACT_READY };
    const uint8_t LO[3] = { PAL_TEXT_FADED };
    if (!row) return ui_clarity_from_evidence(170, 140, 0, HI, LO);
    uint8_t flags = row->cargo_pub[INSPECT_HNN_TRACE_FLAGS];
    const uint8_t WARN[3] = { PAL_ORE_AMBER };
    const uint8_t BAD[3] = { PAL_WARNING };
    if (flags & INSPECT_HNN_TRACE_WARN_NOISY)
        return ui_clarity_from_evidence(row->chain_len, row->grade, 3, WARN, LO);
    if (flags & INSPECT_HNN_TRACE_WARN_LOW_MARGIN)
        return ui_clarity_from_evidence(row->chain_len, 120, 2, WARN, LO);
    if (flags & INSPECT_HNN_TRACE_WARN_UNTRAINED)
        return ui_clarity_from_evidence(120, 110, 2, BAD, LO);
    return ui_clarity_from_evidence(row->chain_len, row->grade, 0, HI, LO);
}

static void hud_hnn_trace_primary_label(const NetInspectSnapshotRow *row,
                                        char *out,
                                        size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;
    uint8_t flags = row->cargo_pub[INSPECT_HNN_TRACE_FLAGS];
    float load = hud_hnn_trace_unit(row->cargo_pub[INSPECT_HNN_TRACE_LOAD]);
    float fidelity =
        hud_hnn_trace_unit(row->cargo_pub[INSPECT_HNN_TRACE_FIDELITY]);
    if (flags & INSPECT_HNN_TRACE_WARN_UNTRAINED) {
        snprintf(out, cap, "trace idle  no stored flight memory");
    } else if (flags & INSPECT_HNN_TRACE_WARN_NOISY) {
        snprintf(out, cap, "trace noisy  load %u%% fid %u%%",
                 (unsigned)lroundf(load * 100.0f),
                 (unsigned)lroundf(fidelity * 100.0f));
    } else if (flags & INSPECT_HNN_TRACE_WARN_LOW_MARGIN) {
        snprintf(out, cap, "trace uncertain  margin %+0.2f",
                 hud_hnn_trace_margin(row));
    } else {
        snprintf(out, cap, "trace stable  load %u%% fid %u%%",
                 (unsigned)lroundf(load * 100.0f),
                 (unsigned)lroundf(fidelity * 100.0f));
    }
}

static void hud_hnn_trace_detail_label(const NetInspectSnapshotRow *row,
                                       char *out,
                                       size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;
    float load = hud_hnn_trace_unit(row->cargo_pub[INSPECT_HNN_TRACE_LOAD]);
    float snr = hud_hnn_trace_snr(row);
    float margin = hud_hnn_trace_margin(row);
    uint16_t capacity =
        (uint16_t)row->cargo_pub[INSPECT_HNN_TRACE_CAPACITY_LO] |
        ((uint16_t)row->cargo_pub[INSPECT_HNN_TRACE_CAPACITY_HI] << 8);
    snprintf(out, cap, "load %u%%  stored %u/%u  snr %.1f  margin %+0.2f",
             (unsigned)lroundf(load * 100.0f),
             (unsigned)row->quantity,
             (unsigned)(capacity ? capacity : HNN_TRACE_CAPACITY),
             snr, margin);
}

static void hud_hnn_trace_contract_label(const NetInspectSnapshotRow *row,
                                         char *out,
                                         size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;
    uint16_t dim = hud_read_u16_le(
        &row->cargo_pub[INSPECT_HNN_TRACE_DIM_LO]);
    if (dim == 0)
        dim = (uint16_t)hud_read_u16_le(&row->receipt_head[16]);
    uint8_t keygen = row->cargo_pub[INSPECT_HNN_TRACE_KEYGEN_VERSION];
    uint8_t encoder = row->cargo_pub[INSPECT_HNN_TRACE_ENCODER_VERSION];
    uint8_t format = row->cargo_pub[INSPECT_HNN_TRACE_FORMAT_VERSION];
    uint64_t seed = hud_read_u64_le(&row->receipt_head[0]);
    uint64_t vocab = row->event_id ? row->event_id
                                   : hud_read_u64_le(&row->receipt_head[8]);
    snprintf(out, cap, "d%u k%u/e%u/t%u seed %04llx vocab %04llx",
             (unsigned)(dim ? dim : HNN_DIM),
             (unsigned)keygen,
             (unsigned)encoder,
             (unsigned)format,
             (unsigned long long)(seed & 0xffffull),
             (unsigned long long)(vocab & 0xffffull));
}

static void hud_job_top_signal_label(const NetInspectSnapshotRow *row,
                                     char *out,
                                     size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row) return;

    int best_a = -1;
    int best_b = -1;
    uint8_t score_a = 0;
    uint8_t score_b = 0;
    for (int i = 0; i < INSPECT_JOB_FACTOR_COUNT; i++) {
        uint8_t score = row->cargo_pub[i];
        if (score > score_a) {
            best_b = best_a;
            score_b = score_a;
            best_a = i;
            score_a = score;
        } else if (score > score_b) {
            best_b = i;
            score_b = score;
        }
    }
    if (best_a < 0 || score_a == 0) {
        snprintf(out, cap, "signals pending");
    } else if (best_b >= 0 && score_b > 32) {
        snprintf(out, cap, "signals %s + %s",
                 hud_job_factor_name(best_a),
                 hud_job_factor_name(best_b));
    } else {
        snprintf(out, cap, "signals %s", hud_job_factor_name(best_a));
    }
}

static const char *hud_contract_action_short(uint8_t action) {
    switch ((contract_action_t)action) {
    case CONTRACT_TRACTOR:  return "haul";
    case CONTRACT_FRACTURE: return "fracture";
    case CONTRACT_DELIVERY: return "deliver";
    default:                return "act";
    }
}

static bool hash32_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static void hud_hash_short_label(const uint8_t hash[32], char out[8]) {
    if (hash32_is_zero(hash)) {
        out[0] = '\0';
        return;
    }
    mining_callsign_from_pubkey(hash, out);
}

static void hud_hash_detail_label(const uint8_t hash[32],
                                  char *out,
                                  size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (hash32_is_zero(hash)) return;
    if (cap < 17) {
        hud_hash_short_label(hash, out);
        return;
    }
    snprintf(out, cap, "%02x%02x%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3],
             hash[4], hash[5], hash[6], hash[7]);
}

static void hud_cargo_label(const uint8_t pub[32], char out[12]) {
    if (hash32_is_zero(pub)) {
        out[0] = '\0';
        return;
    }
    mining_render_callsign(pub, out);
}

/* ----- scramble-resolve glyph animation -----
 * Tape-printer feel: when a row's content changes, each character starts
 * as a random glyph and resolves to its true value at t0 + i * stagger.
 * Once settled, the chars stay still — this is not a perpetual jitter.
 *
 * The codebase calls it "hash short label" but mining_callsign_from_pubkey
 * emits base58 — that's the only alphabet we scramble over today, so
 * the truth and the in-flight glyphs share a character class. */
#define HUD_SCRAMBLE_TOTAL_MS    250.0f   /* full settle time per row */
#define HUD_SCRAMBLE_STAGGER_MS   30.0f   /* per-character delay */

static char hud_random_glyph(uint32_t *rng) {
    /* tiny xorshift so the scramble is stable per-frame for the same
     * t but different across (row, char_index, frame). */
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x ? x : 0xDEADBEEFu;
    static const char b58[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    return b58[x % (sizeof(b58) - 1)];
}

/* Render `truth` (null-terminated) into `out` (cap bytes), scrambling
 * unsettled chars from the base58 alphabet. `phase_ms` is ms since the
 * row's animation t0. `lock` is a bitmask (positions 0..31) of char
 * indices that should NEVER scramble (e.g. the literal "M-" prefix on
 * a cargo callsign). Char i settles at i*HUD_SCRAMBLE_STAGGER_MS;
 * after HUD_SCRAMBLE_TOTAL_MS the whole string is the truth. */
static void hud_scramble_into(char *out, size_t cap,
                              const char *truth,
                              float phase_ms, uint32_t lock_mask, uint32_t seed) {
    if (cap == 0) return;
    size_t L = strlen(truth);
    if (L >= cap) L = cap - 1;
    for (size_t i = 0; i < L; i++) {
        float settle = (float)i * HUD_SCRAMBLE_STAGGER_MS;
        bool locked = (i < 32) && ((lock_mask >> i) & 1u);
        if (locked || phase_ms >= settle) {
            out[i] = truth[i];
            continue;
        }
        /* Mid-flight: emit a random glyph. The rng seed mixes (row,
         * char_index, phase) so successive frames cycle through
         * different glyphs, but a given (row, char, frame) is
         * deterministic — no per-frame RAND state. */
        uint32_t rng = seed ^ ((uint32_t)i * 0x9E3779B1u)
                            ^ (uint32_t)(phase_ms * 1.7f);
        out[i] = hud_random_glyph(&rng);
    }
    out[L] = '\0';
}

/* Cargo callsigns have a literal class prefix that should never
 * scramble — only the hash body should animate.
 * Returns the lock mask covering the prefix chars. */
static uint32_t hud_cargo_prefix_lock(const char *cargo) {
    if (!cargo) return 0;
    size_t L = strlen(cargo);
    /* Patterns: "RATi-XYZ" (5 prefix chars), "M-ABCDEF" (2 prefix chars),
     * "ABCDEFG" (no prefix). */
    if (L >= 5 && cargo[0]=='R' && cargo[1]=='A' && cargo[2]=='T'
        && cargo[3]=='i' && cargo[4]=='-')
        return 0x1Fu;
    if (L >= 2 && cargo[1] == '-')
        return 0x3u;
    return 0;
}

/* hud_row_signature lives in inspect_anim.c so signal_test can link
 * it without sokol. See inspect_anim.h. */

static void hud_callsign_label(const char callsign[8], char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!callsign) return;
    size_t n = 0;
    while (n < 8 && callsign[n] != '\0') n++;
    if (n == 0) return;
    if (n >= cap) n = cap - 1;
    memcpy(out, callsign, n);
    out[n] = '\0';
}

static const char *hud_npc_custody_role_label(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return "MINER";
    case NPC_ROLE_HAULER: return "HAULER";
    case NPC_ROLE_TOW:    return "TOW";
    default:              return "NPC";
    }
}

static void hud_npc_label(const npc_ship_t *npc, int idx, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!npc) {
        snprintf(out, cap, "NPC --");
        return;
    }
    if (npc->session_token[0] == 'N' && npc->session_token[1] == 'P' &&
        npc->session_token[2] == 'C') {
        snprintf(out, cap, "%s N%02u", hud_npc_custody_role_label(npc->role),
                 (unsigned)npc->session_token[5]);
    } else {
        snprintf(out, cap, "%s %02d", hud_npc_custody_role_label(npc->role), idx);
    }
}

static void hud_npc_custody_pubkey(const npc_ship_t *npc,
                                   int npc_slot,
                                   uint8_t out[32]) {
    uint8_t role = npc ? (uint8_t)npc->role : 0;
    uint8_t home = npc ? (uint8_t)npc->home_station : 0xFFu;
    npc_custody_pubkey_from_fields(npc ? npc->session_token : NULL,
                                   npc_slot, role, home, out);
}

/* Resolve a 32-byte receipt identity for player-facing provenance. Stations
 * sign receipt links, so they get first claim. Player and worker identities
 * are local labels only: they make the provenance readable without changing
 * the station-signed receipt authority. Unknown identities stay as short hash
 * labels rather than pretending we know more than we do. */
static void hud_identity_name_for_pubkey(const uint8_t pub[32],
                                         char *out, size_t cap) {
    if (cap == 0) return;
    if (!hash32_is_zero(pub)) {
        for (int i = 0; i < MAX_STATIONS; i++) {
            const station_t *st = &g.world.stations[i];
            if (!station_exists(st)) continue;
            if (memcmp(st->station_pubkey, pub, 32) == 0) {
                snprintf(out, cap, "%s", st->name);
                return;
            }
        }
        for (int i = 0; i < MAX_PLAYERS; i++) {
            const server_player_t *sp = &g.world.players[i];
            if (!sp->pubkey_set) continue;
            if (memcmp(sp->pubkey, pub, 32) != 0) continue;
            char callsign[12];
            hud_callsign_label(sp->callsign, callsign, sizeof(callsign));
            if (callsign[0])
                snprintf(out, cap, "%s", callsign);
            else
                snprintf(out, cap, "pilot %d", i);
            return;
        }
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            const npc_ship_t *npc = &g.world.npc_ships[i];
            if (!npc->active) continue;
            bool has_token = false;
            for (int b = 0; b < 8; b++) {
                if (npc->session_token[b] != 0) {
                    has_token = true;
                    break;
                }
            }
            if (!has_token) continue;
            uint8_t custody[32];
            hud_npc_custody_pubkey(npc, i, custody);
            if (memcmp(custody, pub, 32) != 0) continue;
            hud_npc_label(npc, i, out, cap);
            return;
        }
    }
    char tmp[8];
    hud_hash_short_label(pub, tmp);
    snprintf(out, cap, "%s", tmp);
}

/* Compatibility wrapper for existing provenance callsites. */
static void hud_station_name_for_pubkey(const uint8_t pub[32],
                                        char *out, size_t cap) {
    hud_identity_name_for_pubkey(pub, out, cap);
}

static const contract_t *hud_tracked_tractor_contract(void) {
    if (g.tracked_contract < 0 || g.tracked_contract >= MAX_CONTRACTS)
        return NULL;
    if (!(g.player_known_contract_mask & (1u << g.tracked_contract)))
        return NULL; /* dropped from gossip memory */
    const contract_t *contract = &g.world.contracts[g.tracked_contract];
    if (!contract->active || contract->action != CONTRACT_TRACTOR)
        return NULL;
    return contract;
}

static bool hud_receipt_link_line_named(const NetInspectSnapshotRow *link,
                                        char *out,
                                        size_t cap)
{
    if (!link || !out || cap == 0) return false;
    char author[24];
    char recipient[24];
    hud_identity_name_for_pubkey(link->origin_station, author, sizeof(author));
    hud_identity_name_for_pubkey(link->latest_station, recipient, sizeof(recipient));
    return inspect_label_receipt_link_line_named(link, author, recipient, out, cap);
}

static int hud_receipt_link_lines_for_cause_page(
    const NetInspectSnapshot *snap,
    const InspectJobCause *cause,
    int skip_lines,
    char out[][64],
    int max_lines)
{
    if (!snap || !cause || !cause->receipt || !out || max_lines <= 0)
        return 0;
    for (int i = 0; i < max_lines; i++)
        out[i][0] = '\0';
    if (skip_lines < 0) skip_lines = 0;

    bool receipt_seen = false;
    int count = 0;
    int seen_links = 0;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        if (row == cause->receipt) {
            receipt_seen = true;
            continue;
        }
        if (!receipt_seen) continue;
        if (!(row->flags & INSPECT_ROW_DIAGNOSTIC) ||
            row->commodity != (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
            continue;
        }
        if (seen_links++ < skip_lines) continue;
        if (hud_receipt_link_line_named(row, out[count], 64))
            count++;
        if (count >= max_lines) break;
    }
    return count;
}

static bool hud_inspect_ticker_row_label(const NetInspectSnapshot *snap,
                                         const NetInspectSnapshotRow *row,
                                         char *out,
                                         size_t cap,
                                         ui_clarity_t *clarity_out)
{
    if (!snap || !row || !out || cap == 0) return false;
    out[0] = '\0';
    ui_clarity_t clarity = ui_clarity_from_evidence(
        row->grade ? row->grade : 180,
        row->chain_len ? row->chain_len : 160,
        0,
        (const uint8_t[3]){ PAL_CONTRACT_READY },
        (const uint8_t[3]){ PAL_TEXT_FADED });

    if (row->flags & INSPECT_ROW_DIAGNOSTIC) {
        if (row->commodity == (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
            char head[16];
            hud_hash_short_label(row->receipt_head, head);
            snprintf(out, cap, "receipt link  head %s  ev %llu",
                     head[0] ? head : "--------",
                     (unsigned long long)row->event_id);
            if (clarity_out) *clarity_out = clarity;
            return true;
        }
        if (row->commodity == (uint8_t)INSPECT_DIAG_HNN_TRACE) {
            hud_hnn_trace_primary_label(row, out, cap);
            if (clarity_out) *clarity_out = hud_hnn_trace_clarity(row);
            return true;
        }

        uint8_t station_a = (uint8_t)(row->event_id & 0xFFu);
        uint8_t station_b = (uint8_t)((row->event_id >> 8) & 0xFFu);
        uint8_t action = (uint8_t)((row->event_id >> 16) & 0xFFu);
        uint8_t commodity = (uint8_t)((row->event_id >> 24) & 0xFFu);
        const char *src = (station_a < MAX_STATIONS)
            ? g.world.stations[station_a].name : "?";
        const char *dst = (station_b < MAX_STATIONS)
            ? g.world.stations[station_b].name : "?";
        const char *comm = (commodity < COMMODITY_COUNT)
            ? commodity_code((commodity_t)commodity) : "GEN";
        bool job_row = hud_inspect_diag_is_job(row->commodity);
        if (job_row) {
            char reason[48];
            hud_job_reason_label(row, reason, sizeof(reason));
            clarity = hud_job_clarity(row);
            snprintf(out, cap, "%s %s  %.10s > %.10s  %s",
                     hud_inspect_diag_label(row->commodity),
                     comm, src, dst, reason[0] ? reason : "signals pending");
        } else {
            snprintf(out, cap, "%s %s  %s %.12s",
                     hud_inspect_diag_label(row->commodity),
                     comm,
                     hud_contract_action_short(action),
                     src);
        }
        if (clarity_out) *clarity_out = clarity;
        return true;
    }

    const char *comm = (row->commodity < COMMODITY_COUNT)
        ? commodity_code((commodity_t)row->commodity) : "cargo";
    unsigned qty = row->quantity > 0 ? row->quantity : 1;
    char cargo[12];
    hud_cargo_label(row->cargo_pub, cargo);
    if (cargo[0]) {
        snprintf(out, cap, "cargo %s %s x%u  %s",
                 hud_grade_short_label(row->grade), comm, qty, cargo);
    } else {
        snprintf(out, cap, "cargo %s %s x%u",
                 hud_grade_short_label(row->grade), comm, qty);
    }
    if (clarity_out) *clarity_out = clarity;
    return true;
}

static int hud_inspect_ticker_pick_row(const NetInspectSnapshot *snap,
                                       int cycle)
{
    if (!snap || snap->row_count <= 0) return -1;
    int diag_count = 0;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        if (snap->rows[i].flags & INSPECT_ROW_DIAGNOSTIC)
            diag_count++;
    }
    int eligible = diag_count > 0 ? diag_count : snap->row_count;
    if (eligible <= 0) return -1;
    int wanted = cycle % eligible;
    if (wanted < 0) wanted += eligible;
    for (int i = 0; i < snap->row_count && i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        bool use = diag_count > 0
            ? ((snap->rows[i].flags & INSPECT_ROW_DIAGNOSTIC) != 0)
            : true;
        if (!use) continue;
        if (wanted-- == 0) return i;
    }
    return -1;
}

static void hud_draw_npc_memory_ticker(const NetInspectSnapshot *snap,
                                       int target_idx,
                                       float screen_w,
                                       float screen_h)
{
    if (!snap || target_idx < 0 || target_idx >= MAX_NPC_SHIPS) return;
    const npc_ship_t *npc = &g.world.npc_ships[target_idx];
    if (!npc->active) return;

    float cell = 8.0f;
    float px = fmaxf(14.0f, screen_w - 340.0f);
    float py = (screen_h < 520.0f) ? 70.0f : 86.0f;
    float bg_x = fmaxf(8.0f, px - 10.0f);
    float bg_y = fmaxf(8.0f, py - 9.0f);
    float bg_w = fminf(326.0f, screen_w - bg_x - 12.0f);
    float bg_h = 76.0f;
    int text_chars = (int)floorf((bg_w - 20.0f) / cell);

    int cycle = (int)floorf(g.world.time / 0.85f);
    InspectJobCause cause;
    bool has_job = inspect_label_find_job_cause(snap, &cause);
    const NetInspectSnapshotRow *job = has_job ? cause.job : NULL;
    const NetInspectSnapshotRow *hnn_trace = hud_inspect_find_hnn_trace(snap);
    if (hnn_trace) bg_h = 90.0f;

    int row_idx = has_job ? -1 : hud_inspect_ticker_pick_row(snap, cycle);
    char memory[96];
    char job_card_action[96] = {0};
    char job_card_source[96] = {0};
    ui_clarity_t clarity = hud_job_clarity(NULL);
    if (job) {
        clarity = hud_job_clarity(job);
        uint8_t source_idx = (uint8_t)(job->event_id & 0xFFu);
        uint8_t dest_idx = (uint8_t)((job->event_id >> 8) & 0xFFu);
        uint8_t commodity = (uint8_t)((job->event_id >> 24) & 0xFFu);
        const char *job_source = (source_idx < MAX_STATIONS)
            ? g.world.stations[source_idx].name : "?";
        const char *job_dest = (dest_idx < MAX_STATIONS)
            ? g.world.stations[dest_idx].name : "?";
        const char *comm = (commodity < COMMODITY_COUNT)
            ? commodity_short_name((commodity_t)commodity) : "work";
        if (!inspect_label_job_contact_card(
                job, comm, job_source, job_dest,
                job_card_action, sizeof(job_card_action),
                memory, sizeof(memory),
                job_card_source, sizeof(job_card_source))) {
            hud_job_reason_label(job, memory, sizeof(memory));
        }
    } else if (hnn_trace) {
        hud_hnn_trace_primary_label(hnn_trace, memory, sizeof(memory));
        clarity = hud_hnn_trace_clarity(hnn_trace);
    } else if (row_idx >= 0) {
        hud_inspect_ticker_row_label(snap, &snap->rows[row_idx],
                                     memory, sizeof(memory), &clarity);
    } else {
        snprintf(memory, sizeof(memory), "memory quiet  awaiting dock gossip");
        clarity = ui_clarity_from_evidence(
            150, 120, 1,
            (const uint8_t[3]){ PAL_CONTRACT_READY },
            (const uint8_t[3]){ PAL_TEXT_FADED });
    }

    char memory_seen[96];
    ui_clarity_degrade_text(memory, clarity.clarity,
                            (uint32_t)(target_idx * 2654435761u + cycle),
                            memory_seen, sizeof(memory_seen));

    static const char stream_chars[] = "|/-\\:*+.";
    char stream = stream_chars[((int)(g.world.time * 14.0f) + target_idx) %
                               ((int)sizeof(stream_chars) - 1)];

    hud_draw_alpha_rect(bg_x, bg_y, bg_w, bg_h,
                        0.02f, 0.015f, 0.025f, 0.54f);
    hud_draw_alpha_rect(bg_x, bg_y, 3.0f, bg_h,
                        0.18f, 0.55f, 0.75f, 0.38f);

    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);

    char npc_name[24];
    char npc_seen[24];
    hud_npc_label(npc, target_idx, npc_name, sizeof(npc_name));
    ui_clarity_degrade_text(npc_name, clarity.clarity,
                            (uint32_t)(target_idx * 977u + snap->role),
                            npc_seen, sizeof(npc_seen));

    sdtx_pos(px / cell, py / cell);
    sdtx_color3b(clarity.fg[0], clarity.fg[1], clarity.fg[2]);
    sdtx_printf("[ CONTACT %s ]", npc_seen);

    sdtx_pos(px / cell, (py + 14.0f) / cell);
    sdtx_color3b(PAL_INSPECT_STATION);
    const char *home = (snap->home_station < MAX_STATIONS)
        ? g.world.stations[snap->home_station].name : "?";
    const char *dest = (snap->dest_station < MAX_STATIONS)
        ? g.world.stations[snap->dest_station].name : "?";
    if (job) {
        char action_fit[96];
        hud_fit_text(job_card_action[0] ? job_card_action : "selected work",
                     text_chars, action_fit, sizeof(action_fit));
        sdtx_puts(action_fit);
    } else {
        sdtx_printf("%s %s  %.10s > %.10s",
                    hud_npc_role_label(snap->role),
                    hud_npc_state_label(snap->state),
                    home, dest);
    }

    sdtx_pos(px / cell, (py + 32.0f) / cell);
    sdtx_color3b(job ? clarity.fg[0] : clarity.dim[0],
                 job ? clarity.fg[1] : clarity.dim[1],
                 job ? clarity.fg[2] : clarity.dim[2]);
    if (job) {
        sdtx_printf("%s %s  home %.10s",
                    hud_npc_role_label(snap->role),
                    hud_npc_state_label(snap->state),
                    home);
    } else {
        sdtx_printf("%c MEMORY", stream);
    }

    sdtx_pos(px / cell, (py + 46.0f) / cell);
    sdtx_color3b(clarity.fg[0], clarity.fg[1], clarity.fg[2]);
    char memory_fit[96];
    hud_fit_text(memory_seen, text_chars, memory_fit, sizeof(memory_fit));
    sdtx_puts(memory_fit);

    sdtx_pos(px / cell, (py + 60.0f) / cell);
    sdtx_color3b(clarity.dim[0], clarity.dim[1], clarity.dim[2]);
    if (job) {
        char source_chain[72];
        if (job_card_source[0]) {
            char source_seen[96];
            ui_clarity_degrade_text(job_card_source, clarity.clarity,
                                    (uint32_t)(job->event_id & 0xffffffffu),
                                    source_seen, sizeof(source_seen));
            char source_fit[96];
            hud_fit_text(source_seen, text_chars,
                         source_fit, sizeof(source_fit));
            sdtx_puts(source_fit);
        } else if (hud_job_source_chain_label(job, source_chain, sizeof(source_chain))) {
            char source_seen[72];
            ui_clarity_degrade_text(source_chain, clarity.clarity,
                                    (uint32_t)(job->event_id & 0xffffffffu),
                                    source_seen, sizeof(source_seen));
            char source_fit[72];
            hud_fit_text(source_seen, text_chars,
                         source_fit, sizeof(source_fit));
            sdtx_puts(source_fit);
        } else if (hnn_trace) {
            char trace[96];
            hud_hnn_trace_detail_label(hnn_trace, trace, sizeof(trace));
            sdtx_printf("%s", trace);
        } else {
            sdtx_printf("%s %s", clarity.meter, clarity.word);
        }
    } else if (hnn_trace) {
        char trace[96];
        hud_hnn_trace_detail_label(hnn_trace, trace, sizeof(trace));
        sdtx_printf("%s", trace);
    } else {
        sdtx_printf("%s %s", clarity.meter, clarity.word);
    }
    if (hnn_trace) {
        char contract[96];
        hud_hnn_trace_contract_label(hnn_trace, contract, sizeof(contract));
        sdtx_pos(px / cell, (py + 74.0f) / cell);
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_puts(contract);
    }
}

bool hud_npc_motive_perception_summary(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const NetInspectSnapshot *snap = &g.inspect_snapshot;
    if (g.inspect_snapshot_timer <= 0.0f ||
        snap->target_type != INSPECT_TARGET_NPC ||
        snap->target_index >= MAX_NPC_SHIPS ||
        !g.world.npc_ships[snap->target_index].active) {
        return false;
    }

    InspectJobCause cause;
    if (!inspect_label_find_job_cause(snap, &cause) || !cause.job)
        return false;
    const NetInspectSnapshotRow *job = cause.job;
    uint8_t source_idx = (uint8_t)(job->event_id & 0xFFu);
    uint8_t dest_idx = (uint8_t)((job->event_id >> 8) & 0xFFu);
    uint8_t commodity = (uint8_t)((job->event_id >> 24) & 0xFFu);
    const char *source = source_idx < MAX_STATIONS
        ? g.world.stations[source_idx].name : "?";
    const char *dest = dest_idx < MAX_STATIONS
        ? g.world.stations[dest_idx].name : "?";
    const char *comm = commodity < COMMODITY_COUNT
        ? commodity_short_name((commodity_t)commodity) : "work";

    char action[96];
    char motive[96];
    char source_line[96];
    if (!inspect_label_job_contact_card(job, comm, source, dest,
                                        action, sizeof(action),
                                        motive, sizeof(motive),
                                        source_line, sizeof(source_line))) {
        return false;
    }

    ui_clarity_t clarity = hud_job_clarity(job);
    int cycle = (int)floorf(g.world.time / 0.85f);
    char motive_seen[96];
    char source_seen[96];
    ui_clarity_degrade_text(
        motive, clarity.clarity,
        (uint32_t)(snap->target_index * 2654435761u + cycle),
        motive_seen, sizeof(motive_seen));
    ui_clarity_degrade_text(source_line, clarity.clarity,
                            (uint32_t)(job->event_id & 0xffffffffu),
                            source_seen, sizeof(source_seen));
    snprintf(out, out_size, "%s | %s | %s", action, motive_seen,
             source_seen);
    return true;
}

static void hud_draw_inspect_snapshot_pane(float screen_w, float screen_h) {
    if (g.inspect_snapshot_timer <= 0.0f) return;
    if (LOCAL_PLAYER.docked) return;
    if (g.death_cinematic.active) return;
    /* The timer alone gates visibility — the panel lingers for a few
     * seconds after scan release so the player can read what they
     * just locked onto. */
    const NetInspectSnapshot *snap = &g.inspect_snapshot;
    bool target_is_npc = snap->target_type == INSPECT_TARGET_NPC;
    bool target_is_player = snap->target_type == INSPECT_TARGET_PLAYER;
    if (!target_is_npc && !target_is_player) return;
    int target_idx = (snap->target_index == 0xFFu) ? -1 : (int)snap->target_index;
    if (target_is_npc) {
        if (target_idx < 0 || target_idx >= MAX_NPC_SHIPS) return;
        if (!g.world.npc_ships[target_idx].active) return;
        hud_draw_npc_memory_ticker(snap, target_idx, screen_w, screen_h);
        return;
    } else {
        if (target_idx < 0 || target_idx >= MAX_PLAYERS) return;
        const NetPlayerState *np = hud_net_player_state(target_idx);
        if (!np && !g.world.players[target_idx].connected) return;
    }

    float px = fmaxf(16.0f, screen_w - 360.0f);
    float py = (screen_h < 520.0f) ? 76.0f : 92.0f;
    float cell = 8.0f;
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);

    const contract_t *tracked_contract = hud_tracked_tractor_contract();
    int max_rows = (screen_h < 520.0f) ? 5 : 8;
    if (tracked_contract && max_rows > 1) max_rows--;
    InspectJobCause cause;
    char detail_line1[64];
    char detail_line2[64];
    char detail_line3[64];
    char detail_line4[64];
    char detail_line5[64];
    char detail_line6[64];
    char link_lines[8][64];
    int link_line_count = 0;
    char link_page_line[64];
    bool has_link_page_line = false;
    bool has_detail = false;
    bool receipt_browser = g.inspect_receipt_browser;
    const NetInspectSnapshotRow *contact_job_row = NULL;
    int receipt_links_per_page = receipt_browser
        ? ((screen_h < 560.0f) ? 5 : 8)
        : 3;
    if (target_is_npc && inspect_label_find_job_cause(snap, &cause)) {
        contact_job_row = cause.job;
        const char *cause_station = "?";
        uint8_t station_idx = cause.job
            ? cause.job->cargo_pub[INSPECT_JOB_META_SOURCE_STATION]
            : 0xffu;
        if (station_idx < MAX_STATIONS)
            cause_station = g.world.stations[station_idx].name;
        has_detail = inspect_label_job_detail_lines(
            &cause, cause_station,
            detail_line1, sizeof(detail_line1),
            detail_line2, sizeof(detail_line2),
            detail_line3, sizeof(detail_line3),
            detail_line4, sizeof(detail_line4),
            detail_line5, sizeof(detail_line5),
            detail_line6, sizeof(detail_line6));
        if (has_detail) {
            int link_total = cause.receipt_link_count;
            if (link_total > 0) {
                int page_count =
                    (link_total + receipt_links_per_page - 1) /
                    receipt_links_per_page;
                if (page_count < 1) page_count = 1;
                if (g.inspect_receipt_page >= (uint8_t)page_count)
                    g.inspect_receipt_page =
                        (uint8_t)(g.inspect_receipt_page % (uint8_t)page_count);
                int page = (int)g.inspect_receipt_page;
                int start = page * receipt_links_per_page;
                link_line_count = hud_receipt_link_lines_for_cause_page(
                    snap, &cause, start, link_lines,
                    receipt_links_per_page);
                if (receipt_browser || page_count > 1) {
                    int end = start + link_line_count;
                    if (end > link_total) end = link_total;
                    if (page_count > 1) {
                        inspect_label_receipt_browser_footer(
                            link_total, start, end - start, true,
                            link_page_line, sizeof(link_page_line));
                    } else if (receipt_browser) {
                        inspect_label_receipt_browser_footer(
                            link_total, start, end - start, false,
                            link_page_line, sizeof(link_page_line));
                    }
                    has_link_page_line = true;
                }
            } else if (receipt_browser) {
                inspect_label_receipt_browser_footer(
                    0, 0, 0, false, link_page_line, sizeof(link_page_line));
                has_link_page_line = true;
            }
        }
    }
    if (!has_detail)
        receipt_browser = false;
    if (has_detail && max_rows > 3)
        max_rows -= 2;
    if (has_link_page_line && max_rows > 2)
        max_rows--;
    for (int i = 0; i < link_line_count && max_rows > 2; i++)
        max_rows--;
    if (receipt_browser)
        max_rows = 0;
    int rows = snap->row_count;
    if (rows > max_rows) rows = max_rows;
    unsigned visible_units = 0;
    for (int i = 0; i < rows; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        if (row->flags & INSPECT_ROW_DIAGNOSTIC) continue;
        visible_units += row->quantity > 0 ? row->quantity : 1;
    }
    bool has_more_units = snap->manifest_count > visible_units;
    {
        float bg_x = fmaxf(8.0f, px - 12.0f);
        float bg_y = fmaxf(8.0f, py - 10.0f);
        float bg_w = fmaxf(340.0f, screen_w - bg_x - 16.0f);
        float row_stride = 64.0f;
        float bg_h = (snap->manifest_count == 0 && rows == 0)
            ? 72.0f
            : 58.0f + (float)rows * row_stride + (has_more_units ? 22.0f : 0.0f);
        if (has_detail) {
            bg_h += 94.0f + (float)link_line_count * 12.0f +
                    (has_link_page_line ? 12.0f : 0.0f);
            if (receipt_browser)
                bg_h += 18.0f;
        }
        bg_h = fminf(bg_h, screen_h - bg_y - 16.0f);
        hud_draw_alpha_rect(bg_x, bg_y, bg_w, bg_h, 0.02f, 0.015f, 0.025f, 0.58f);
        hud_draw_alpha_rect(bg_x, bg_y, 3.0f, bg_h, 0.18f, 0.55f, 0.75f, 0.34f);
    }

    sdtx_pos(px / cell, py / cell);
    ui_clarity_t contact_clarity = target_is_npc
        ? hud_job_clarity(contact_job_row)
        : ui_clarity_from_evidence(235, 235, 0,
                                   (const uint8_t[3]){ PAL_ORE_AMBER },
                                   (const uint8_t[3]){ PAL_TEXT_FADED });
    sdtx_color3b(contact_clarity.fg[0], contact_clarity.fg[1],
                 contact_clarity.fg[2]);
    if (target_is_npc) {
        char npc_name[24];
        char npc_seen[24];
        const npc_ship_t *npc = &g.world.npc_ships[target_idx];
        hud_npc_label(npc, target_idx, npc_name, sizeof(npc_name));
        ui_clarity_degrade_text(npc_name, contact_clarity.clarity,
                                (uint32_t)(target_idx * 977u + snap->role),
                                npc_seen, sizeof(npc_seen));
        sdtx_printf("[ CONTACT %s ]", npc_seen);
    } else {
        char pilot[16];
        hud_player_scan_label(target_idx, pilot, sizeof(pilot));
        sdtx_printf("[ PILOT %s ]", pilot);
    }

    sdtx_pos(px / cell, (py + 14.0f) / cell);
    sdtx_color3b(PAL_INSPECT_STATION);
    const char *home = (snap->home_station < MAX_STATIONS)
        ? g.world.stations[snap->home_station].name : "?";
    const char *dest = (snap->dest_station < MAX_STATIONS)
        ? g.world.stations[snap->dest_station].name : "?";
    if (target_is_npc) {
        char home_seen[24];
        char dest_seen[24];
        ui_clarity_degrade_text(home, contact_clarity.clarity,
                                (uint32_t)(snap->home_station * 131u + 7u),
                                home_seen, sizeof(home_seen));
        ui_clarity_degrade_text(dest, contact_clarity.clarity,
                                (uint32_t)(snap->dest_station * 173u + 11u),
                                dest_seen, sizeof(dest_seen));
        sdtx_printf("%s %s  %.12s > %.12s",
                    hud_npc_role_label(snap->role),
                    hud_npc_state_label(snap->state), home_seen, dest_seen);
    } else {
        sdtx_printf("%s  hull %u  near %.12s",
                    hud_hull_class_label(snap->role),
                    (unsigned)snap->state, dest);
    }

    sdtx_pos(px / cell, (py + 28.0f) / cell);
    if (target_is_npc)
        sdtx_color3b(contact_clarity.dim[0], contact_clarity.dim[1],
                     contact_clarity.dim[2]);
    else
        sdtx_color3b(PAL_TEXT_GREY);
    sdtx_printf("manifest %u unit%s",
                (unsigned)snap->manifest_count,
                snap->manifest_count == 1 ? "" : "s");
    if (target_is_npc)
        sdtx_printf("  %s %s", contact_clarity.meter, contact_clarity.word);

    if (snap->manifest_count == 0 && rows == 0) {
        sdtx_pos(px / cell, (py + 44.0f) / cell);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_puts(target_is_player ? "no cargo aboard" : "no cargo in custody");
        return;
    }

    float next_y = py + 48.0f;
    if (has_detail) {
        sdtx_pos(px / cell, next_y / cell);
        sdtx_color3b(PAL_CONTRACT_READY);
        sdtx_puts(receipt_browser ? "RECEIPT RELAY" : "JOB DETAIL");
        if (receipt_browser) {
            sdtx_pos(px / cell, (next_y + 12.0f) / cell);
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_puts("local proof view for selected worker job");
            next_y += 18.0f;
        }
        sdtx_pos(px / cell, (next_y + 12.0f) / cell);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_puts(detail_line1);
        sdtx_pos(px / cell, (next_y + 24.0f) / cell);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_puts(detail_line2);
        sdtx_pos(px / cell, (next_y + 36.0f) / cell);
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_puts(detail_line3);
        sdtx_pos(px / cell, (next_y + 48.0f) / cell);
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_puts(detail_line4);
        sdtx_pos(px / cell, (next_y + 60.0f) / cell);
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_puts(detail_line5);
        sdtx_pos(px / cell, (next_y + 72.0f) / cell);
        sdtx_color3b(PAL_TEXT_FADED);
        sdtx_puts(detail_line6);
        for (int li = 0; li < link_line_count; li++) {
            sdtx_pos(px / cell, (next_y + 84.0f + (float)li * 12.0f) / cell);
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_puts(link_lines[li]);
        }
        if (has_link_page_line) {
            sdtx_pos(px / cell,
                     (next_y + 84.0f + (float)link_line_count * 12.0f) / cell);
            sdtx_color3b(PAL_CONTRACT_STATUS);
            sdtx_puts(link_page_line);
        }
        next_y += 94.0f + (float)link_line_count * 12.0f +
                  (has_link_page_line ? 12.0f : 0.0f);
    }
    float now = g.world.time;
    for (int i = 0; i < rows; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        if (row->flags & INSPECT_ROW_DIAGNOSTIC) {
            if (row->commodity == (uint8_t)INSPECT_DIAG_RECEIPT_LINK) {
                char author[20];
                char recipient[20];
                char head[20];
                hud_hash_detail_label(row->origin_station, author, sizeof(author));
                hud_hash_detail_label(row->latest_station, recipient, sizeof(recipient));
                hud_hash_detail_label(row->receipt_head, head, sizeof(head));
                float y = next_y;
                sdtx_pos(px / cell, y / cell);
                sdtx_color3b(PAL_CONTRACT_READY);
                sdtx_printf("receipt link %u/%u",
                            (unsigned)row->grade,
                            (unsigned)row->chain_len);
                sdtx_pos(px / cell, (y + 12.0f) / cell);
                sdtx_color3b(PAL_TEXT_GREY);
                sdtx_printf("event %llu head %.8s",
                            (unsigned long long)row->event_id,
                            head[0] ? head : "--------");
                sdtx_pos(px / cell, (y + 24.0f) / cell);
                sdtx_color3b(PAL_TEXT_FADED);
                sdtx_printf("auth %.8s > rec %.8s",
                            author[0] ? author : "--------",
                            recipient[0] ? recipient : "--------");
                next_y = y + 42.0f;
                continue;
            }
            if (row->commodity == (uint8_t)INSPECT_DIAG_HNN_TRACE) {
                ui_clarity_t trace_clarity = hud_hnn_trace_clarity(row);
                char primary[96];
                char detail[96];
                char contract[112];
                hud_hnn_trace_primary_label(row, primary, sizeof(primary));
                hud_hnn_trace_detail_label(row, detail, sizeof(detail));
                hud_hnn_trace_contract_label(row, contract, sizeof(contract));
                float y = next_y;
                sdtx_pos(px / cell, y / cell);
                sdtx_color3b(trace_clarity.fg[0], trace_clarity.fg[1],
                             trace_clarity.fg[2]);
                sdtx_puts("memory trace");
                sdtx_pos(px / cell, (y + 12.0f) / cell);
                sdtx_color3b(PAL_TEXT_GREY);
                sdtx_puts(primary);
                sdtx_pos(px / cell, (y + 24.0f) / cell);
                sdtx_color3b(trace_clarity.dim[0], trace_clarity.dim[1],
                             trace_clarity.dim[2]);
                sdtx_puts(detail);
                sdtx_pos(px / cell, (y + 36.0f) / cell);
                sdtx_color3b(PAL_TEXT_FADED);
                sdtx_puts(contract);
                next_y = y + 54.0f;
                continue;
            }
            uint8_t station_a = (uint8_t)(row->event_id & 0xFFu);
            uint8_t station_b = (uint8_t)((row->event_id >> 8) & 0xFFu);
            uint8_t action = (uint8_t)((row->event_id >> 16) & 0xFFu);
            uint8_t commodity = (uint8_t)((row->event_id >> 24) & 0xFFu);
            bool job_row = hud_inspect_diag_is_job(row->commodity);
            const char *station = (station_a < MAX_STATIONS)
                ? g.world.stations[station_a].name : "?";
            const char *source = (station_b < MAX_STATIONS)
                ? g.world.stations[station_b].name : NULL;
            const char *job_source = (station_a < MAX_STATIONS)
                ? g.world.stations[station_a].name : "?";
            const char *job_dest = (station_b < MAX_STATIONS)
                ? g.world.stations[station_b].name : "?";
            const char *comm = (commodity < COMMODITY_COUNT)
                ? commodity_code((commodity_t)commodity) : "GEN";
            float y = next_y;
            sdtx_pos(px / cell, y / cell);
            sdtx_color3b(PAL_CONTRACT_READY);
            if (job_row) {
                ui_clarity_t clarity = hud_job_clarity(row);
                sdtx_printf("%s  %s %.10s",
                            hud_inspect_diag_label(row->commodity),
                            comm,
                            job_source);
                sdtx_color3b(clarity.dim[0], clarity.dim[1], clarity.dim[2]);
            } else {
                sdtx_printf("%s  %s %.12s",
                            hud_inspect_diag_label(row->commodity),
                            comm,
                            station);
            }
            sdtx_pos(px / cell, (y + 12.0f) / cell);
            sdtx_color3b(PAL_TEXT_GREY);
            if (job_row) {
                const char *status = row->chain_len >= 200 ? "selected" : "candidate";
                sdtx_printf("%.10s > %.10s  %s sc%u",
                            job_source, job_dest, status, (unsigned)row->grade);
                sdtx_pos(px / cell, (y + 24.0f) / cell);
                ui_clarity_t clarity = hud_job_clarity(row);
                sdtx_color3b(clarity.fg[0], clarity.fg[1], clarity.fg[2]);
                char reason[48];
                hud_job_reason_label(row, reason, sizeof(reason));
                sdtx_puts(reason);
                char source_chain[72];
                if (hud_job_source_chain_label(row, source_chain,
                                               sizeof(source_chain))) {
                    char source_seen[72];
                    ui_clarity_degrade_text(source_chain, clarity.clarity,
                                            (uint32_t)(row->event_id & 0xffffffffu),
                                            source_seen, sizeof(source_seen));
                    sdtx_pos(px / cell, (y + 36.0f) / cell);
                    sdtx_color3b(clarity.dim[0], clarity.dim[1],
                                 clarity.dim[2]);
                    sdtx_puts(source_seen);
                    sdtx_pos(px / cell, (y + 48.0f) / cell);
                } else {
                    sdtx_pos(px / cell, (y + 36.0f) / cell);
                }
                sdtx_color3b(clarity.dim[0], clarity.dim[1], clarity.dim[2]);
                char signals[48];
                hud_job_top_signal_label(row, signals, sizeof(signals));
                sdtx_printf("%s  %s %s", signals, clarity.meter, clarity.word);
                next_y = y + (source_chain[0] ? 66.0f : 54.0f);
                continue;
            } else if (source) {
                sdtx_printf("%s via %.12s  c%u s%u hint %u",
                            hud_contract_action_short(action), source,
                            (unsigned)row->grade,
                            (unsigned)row->chain_len,
                            (unsigned)row->quantity);
            } else {
                sdtx_printf("%s  c%u s%u hint %u",
                            hud_contract_action_short(action),
                            (unsigned)row->grade,
                            (unsigned)row->chain_len,
                            (unsigned)row->quantity);
            }
            char market_source[72];
            if (hud_market_source_chain_label(row, market_source,
                                              sizeof(market_source))) {
                sdtx_pos(px / cell, (y + 24.0f) / cell);
                sdtx_color3b(PAL_TEXT_FADED);
                sdtx_puts(market_source);
                next_y = y + 42.0f;
            } else {
                next_y = y + 28.0f;
            }
            continue;
        }
        char cargo[12];
        hud_cargo_label(row->cargo_pub, cargo);
        unsigned qty = row->quantity > 0 ? row->quantity : 1;
        bool grouped = (row->flags & INSPECT_ROW_GROUPED) != 0;

        /* On grouped rows, chain_len is repurposed as the prefix_class
         * of the bucket (0 = ANONYMOUS bulk, otherwise a named class). */
        const char *prefix_label = NULL;
        if (grouped) {
            switch ((ingot_prefix_t)row->chain_len) {
            case INGOT_PREFIX_M:            prefix_label = "M class"; break;
            case INGOT_PREFIX_H:            prefix_label = "H class"; break;
            case INGOT_PREFIX_T:            prefix_label = "T class"; break;
            case INGOT_PREFIX_S:            prefix_label = "S class"; break;
            case INGOT_PREFIX_F:            prefix_label = "F class"; break;
            case INGOT_PREFIX_K:            prefix_label = "K class"; break;
            case INGOT_PREFIX_RATI:         prefix_label = "RATi class"; break;
            case INGOT_PREFIX_COMMISSIONED: prefix_label = "RATi*"; break;
            case INGOT_PREFIX_ANONYMOUS:
            default:                        prefix_label = "bulk"; break;
            }
        }

        /* Per-row scramble state: re-trigger the animation only when the
         * row's content fingerprint actually changed. Same content across
         * frames = no re-scramble. */
        uint64_t sig = hud_row_signature(row);
        if (g.inspect_row_anim[i].sig != sig) {
            g.inspect_row_anim[i].sig = sig;
            g.inspect_row_anim[i].anim_t0 = now;
            g.inspect_row_anim[i].phase = 0;
            g.inspect_row_anim[i].phase_t0 = now;
        }
        float row_phase_ms = (now - g.inspect_row_anim[i].anim_t0) * 1000.0f;
        float settle_norm = row_phase_ms / HUD_SCRAMBLE_TOTAL_MS;
        if (settle_norm < 0.0f) settle_norm = 0.0f;
        if (settle_norm > 1.0f) settle_norm = 1.0f;
        /* Brightness pulses during settle: dimmer at the start, full at
         * settle, then a touch under so settled rows don't burn too hot. */
        float row_alpha = 0.55f + 0.45f * settle_norm;
        uint8_t a8_label = (uint8_t)(235.0f * row_alpha);
        uint8_t a8_chain = (uint8_t)(200.0f * row_alpha);

        uint8_t rr, gg, bb;
        mining_grade_rgb((mining_grade_t)row->grade, &rr, &gg, &bb);
        float y = next_y;

        /* The label proper: rarity short + commodity code + cargo/prefix
         * + quantity. The cargo callsign body scrambles but the class
         * prefix ("M-", "RATi-") and the ingot-class words ("M class")
         * stay fixed — those aren't hashes, those are stable bucket
         * labels. */
        char cargo_disp[12];
        if (prefix_label) {
            snprintf(cargo_disp, sizeof(cargo_disp), "%s", prefix_label);
        } else {
            uint32_t lock = hud_cargo_prefix_lock(cargo);
            uint32_t seed = (uint32_t)(sig & 0xffffffffu) ^ 0xC0FFEEu;
            hud_scramble_into(cargo_disp, sizeof(cargo_disp), cargo,
                              row_phase_ms, lock, seed);
        }
        sdtx_pos(px / cell, y / cell);
        sdtx_color4b(rr, gg, bb, a8_label);
        const char *trust_label = hud_cargo_trust_label(row);
        sdtx_printf("%-5s %s %-10s x%u%s%s",
                    hud_grade_short_label(row->grade),
                    commodity_code((commodity_t)row->commodity),
                    cargo_disp,
                    qty,
                    trust_label[0] ? "  " : "",
                    trust_label);

        next_y = y + 14.0f;
        bool drew_contract_fit = false;
        if (tracked_contract) {
            bool has_proof = !grouped &&
                (((row->flags & INSPECT_ROW_HAS_RECEIPT) != 0) ||
                 contract_fit_has_bytes(row->cargo_pub));
            contract_fit_reason_t fit = contract_fit_cargo_fields(
                tracked_contract,
                (commodity_t)row->commodity,
                (mining_grade_t)row->grade,
                (uint16_t)qty,
                has_proof);
            bool relevant = contract_fit_is_ok(fit) ||
                            row->commodity == (uint8_t)tracked_contract->commodity;
            if (relevant) {
                sdtx_pos(px / cell, (y + 12.0f) / cell);
                if (contract_fit_is_ok(fit) && has_proof) {
                    sdtx_color4b(PAL_CONTRACT_READY, a8_chain);
                    sdtx_puts("contract: match");
                } else if (contract_fit_is_ok(fit)) {
                    sdtx_color4b(PAL_CONTRACT_HINT, a8_chain);
                    sdtx_puts("contract: proof missing");
                } else {
                    sdtx_color4b(PAL_TEXT_GREY, a8_chain);
                    sdtx_printf("contract: %s",
                                contract_fit_reason_label(fit));
                }
                next_y = y + 26.0f;
                drew_contract_fit = true;
            }
        }
        if (!grouped && (row->flags & INSPECT_ROW_HAS_RECEIPT)) {
            char head[8], origin[8], latest[8];
            hud_hash_short_label(row->receipt_head, head);
            hud_hash_short_label(row->origin_station, origin);
            hud_hash_short_label(row->latest_station, latest);
            /* Phase rotation for chained rows. Singletons (chain_len==1)
             * pin to phase 0 — origin and latest are the same so phases
             * B/C add nothing. Each phase fires a fresh per-phase
             * scramble on the bits that actually changed line-to-line. */
            const float PHASE_DUR = 1.2f;
            uint8_t target_phase = 0;
            if (row->chain_len > 1) {
                int slot = (int)((now - g.inspect_row_anim[i].anim_t0) / PHASE_DUR) % 3;
                if (slot < 0) slot = 0;
                target_phase = (uint8_t)slot;
            }
            if (target_phase != g.inspect_row_anim[i].phase) {
                g.inspect_row_anim[i].phase = target_phase;
                g.inspect_row_anim[i].phase_t0 = now;
            }
            float phase_ms = (now - g.inspect_row_anim[i].phase_t0) * 1000.0f;

            float chain_y = drew_contract_fit ? (y + 24.0f) : (y + 12.0f);
            sdtx_pos(px / cell, chain_y / cell);
            sdtx_color4b(PAL_TEXT_GREY, a8_chain);
            uint32_t seed = (uint32_t)(sig & 0xffffffffu);
            if (target_phase == 0) {
                /* Phase A — origin>latest + head + ev. The body of each
                 * 7-char hash scrambles independently. */
                char a_origin[8], a_latest[8], a_head[8];
                hud_scramble_into(a_origin, sizeof(a_origin), origin,
                                  phase_ms,0, seed ^ 1u);
                hud_scramble_into(a_latest, sizeof(a_latest), latest,
                                  phase_ms,0, seed ^ 2u);
                hud_scramble_into(a_head, sizeof(a_head), head,
                                  phase_ms,0, seed ^ 3u);
                sdtx_printf("chain %u  %s>%s  head %s  ev %llu",
                            (unsigned)row->chain_len, a_origin, a_latest,
                            a_head, (unsigned long long)row->event_id);
            } else if (target_phase == 1) {
                /* Phase B — origin: <station name or hash>. */
                char name[24];
                hud_station_name_for_pubkey(row->origin_station, name, sizeof(name));
                char disp[24];
                hud_scramble_into(disp, sizeof(disp), name,
                                  phase_ms,0, seed ^ 4u);
                sdtx_printf("chain %u  origin: %s",
                            (unsigned)row->chain_len, disp);
            } else {
                /* Phase C — latest: <station name or hash>. */
                char name[24];
                hud_station_name_for_pubkey(row->latest_station, name, sizeof(name));
                char disp[24];
                hud_scramble_into(disp, sizeof(disp), name,
                                  phase_ms,0, seed ^ 5u);
                sdtx_printf("chain %u  latest: %s",
                            (unsigned)row->chain_len, disp);
            }
            next_y = chain_y + 14.0f;
        } else if (!grouped) {
            float chain_y = drew_contract_fit ? (y + 24.0f) : (y + 12.0f);
            sdtx_pos(px / cell, chain_y / cell);
            sdtx_color4b(PAL_TEXT_GREY, a8_chain);
            if (hash32_is_zero(row->cargo_pub)) {
                sdtx_puts("identity missing");
            } else {
                sdtx_puts("chain: station origin / receipt pending");
            }
            next_y = chain_y + 14.0f;
        }
    }
    /* Clear stale anim slots beyond the current row count so an
     * eventually-shorter snapshot doesn't carry old signatures forward
     * (would otherwise look like a fresh re-trigger when the same
     * NPC's row count grew back). */
    for (int i = rows; i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        g.inspect_row_anim[i].sig = 0;
    }

    if (snap->manifest_count > visible_units) {
        sdtx_pos(px / cell, next_y / cell);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_printf("+%u more unit%s",
                    (unsigned)(snap->manifest_count - visible_units),
                    (snap->manifest_count - visible_units) == 1 ? "" : "s");
    }
}

bool hud_signal_loss_perception_summary(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (g.local_player_slot < 0 || LOCAL_PLAYER.docked) return false;
    float sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
    if (sig_quality < 0.01f) {
        snprintf(out, out_size, "[ SIGNAL LOST ]");
        return true;
    }
    if (sig_quality >= SIGNAL_BAND_OPERATIONAL) return false;
    int ctrl_pct = (int)lroundf(signal_control_scale(sig_quality) * 100.0f);
    snprintf(out, out_size, "[ LOW SIGNAL -- CTRL %d%% ]", ctrl_pct);
    return true;
}

static void hud_draw_signal_lost_warning(float screen_w, float screen_h, float sig_quality) {
    if (LOCAL_PLAYER.docked) return;
    if (g.death_screen_timer > 0.0f || g.death_cinematic.active) return;

    bool lost = sig_quality < 0.01f;
    bool low = sig_quality < SIGNAL_BAND_OPERATIONAL;
    if (!lost && !low) return;

    float blink = sinf(g.world.time * 3.0f);
    if (lost && blink <= 0.0f) return;

    float cell = 8.0f;
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    if (lost) {
        uint8_t ba = (uint8_t)(blink * 200.0f);
        sdtx_color4b(255, 70, 50, ba);
        sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.40f) / cell, cell,
                           "[ SIGNAL LOST ]");
        return;
    }

    float urgency = 1.0f - clampf(sig_quality / SIGNAL_BAND_OPERATIONAL,
                                  0.0f, 1.0f);
    uint8_t alpha = (uint8_t)(90.0f + urgency * 110.0f);
    const uint8_t frontier_rgb[3] = { PAL_SIGNAL_FRONTIER };
    sdtx_color4b(frontier_rgb[0], frontier_rgb[1], frontier_rgb[2], alpha);
    char text[64];
    if (!hud_signal_loss_perception_summary(text, sizeof(text))) return;
    sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.40f) / cell, cell,
                       text);
}

/* Render the post-classify shared panels common to compact + wide.
 * Called once per frame after each layout's top-status section. */
/* Directional hit indicator removed — the rendered triangle didn't
 * read well in practice. Symbol kept (no-op) so the call site doesn't
 * need surgery, and damage_dir_{x,y,timer} keep updating in case a
 * future replacement (audio panning? a 3D-style "incoming" callout?)
 * wants to consume them. The kill-feed + popup + screen shake + audio
 * already cover the "you got hit" telemetry. */
static void hud_draw_hit_indicator(float screen_w, float screen_h) {
    (void)screen_w;
    (void)screen_h;
}

/* PvP kill-feed — single line at top-center, fades on the timer.
 * Server-emitted SIM_EVENT_NPC_KILL drives the text; eventually
 * remote-player kills will share the same surface. */
static void hud_draw_kill_feed(float screen_w, float screen_h) {
    if (g.kill_feed_timer <= 0.0f) return;
    if (g.kill_feed_text[0] == '\0') return;
    float cell = 8.0f;
    /* Fade-in for the first 0.2 s, fade-out for the last 0.5 s. */
    float a = 1.0f;
    if (g.kill_feed_timer > 2.8f) a = (3.0f - g.kill_feed_timer) / 0.2f;
    else if (g.kill_feed_timer < 0.5f) a = g.kill_feed_timer / 0.5f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    uint8_t a8 = (uint8_t)(a * 255.0f);
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color4b(255, 220, 100, a8);  /* warm gold reads as scoreboard */
    sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.08f) / cell, cell,
                       g.kill_feed_text);
    (void)screen_h;
}

/* Killer-side confirm banner — sits one line below the kill feed so
 * both can be visible if a kill confirm and a separate feed event
 * happen close together. Brighter / hotter color to read as an
 * achievement notification rather than a scoreboard entry. */
static void hud_draw_kill_confirm(float screen_w, float screen_h) {
    if (g.kill_confirm_timer <= 0.0f) return;
    if (g.kill_confirm_text[0] == '\0') return;
    float cell = 8.0f;
    float a = 1.0f;
    if (g.kill_confirm_timer > 2.8f) a = (3.0f - g.kill_confirm_timer) / 0.2f;
    else if (g.kill_confirm_timer < 0.5f) a = g.kill_confirm_timer / 0.5f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    uint8_t a8 = (uint8_t)(a * 255.0f);
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color4b(255, 90, 60, a8);
    sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.08f + 18.0f) / cell, cell,
                       g.kill_confirm_text);
}

/* Session kill counter, top-right under the version line. Compact
 * "K 3" so it fits beside the v<hash>/connection status without
 * crowding the alpha banner above. */
static void hud_draw_kill_counter(float screen_w) {
    if (g.kill_count_session <= 0) return;
    float x = ui_text_pos(fmaxf(8.0f, screen_w - 60.0f));
    float y = ui_text_pos(22.0f);
    sdtx_pos(x, y);
    sdtx_color3b(255, 200, 80);
    sdtx_printf("K %d", g.kill_count_session);
}

/* Session scoreboard, toggled with [Tab] while undocked. Kills/deaths
 * are aggregated client-side from observed SIM_EVENT_DEATH /
 * SIM_EVENT_NPC_KILL events. Sorted by kills desc; ties broken by
 * fewer deaths. Single-pass insertion sort -- the row pool is 16. */
static void hud_draw_scoreboard(float screen_w, float screen_h) {
    if (!g.scoreboard.show) return;
    float cell = 8.0f;
    int order[16];
    int n = g.scoreboard.row_count;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i];
        int j = i - 1;
        while (j >= 0) {
            int a = order[j];
            bool worse =
                (g.scoreboard.rows[a].kills < g.scoreboard.rows[k].kills) ||
                (g.scoreboard.rows[a].kills == g.scoreboard.rows[k].kills &&
                 g.scoreboard.rows[a].deaths > g.scoreboard.rows[k].deaths);
            if (!worse) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }

    float panel_w = 280.0f;
    float panel_x = (screen_w - panel_w) * 0.5f;
    float panel_y = screen_h * 0.18f;
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);

    sdtx_color3b(255, 220, 100);
    sdtx_pos(panel_x / cell, panel_y / cell);
    sdtx_puts("== SCOREBOARD ==  [Tab to close]");

    sdtx_color3b(180, 180, 180);
    sdtx_pos(panel_x / cell, (panel_y + 16.0f) / cell);
    sdtx_puts("PILOT             K   D   K/D");

    if (n == 0) {
        sdtx_color3b(140, 140, 140);
        sdtx_pos(panel_x / cell, (panel_y + 32.0f) / cell);
        sdtx_puts("(no kills or deaths yet)");
        return;
    }
    for (int rank = 0; rank < n; rank++) {
        int idx = order[rank];
        const char *label = g.scoreboard.rows[idx].label[0]
                          ? g.scoreboard.rows[idx].label : "????";
        bool is_me = memcmp(g.scoreboard.rows[idx].token,
                            g.world.players[g.local_player_slot].session_token, 8) == 0;
        if (is_me) sdtx_color3b(255, 220, 100);
        else       sdtx_color3b(200, 200, 200);
        char kdr[8];
        if (g.scoreboard.rows[idx].deaths == 0) {
            snprintf(kdr, sizeof(kdr), "%d.0", g.scoreboard.rows[idx].kills);
        } else {
            float r = (float)g.scoreboard.rows[idx].kills /
                      (float)g.scoreboard.rows[idx].deaths;
            snprintf(kdr, sizeof(kdr), "%.2f", r);
        }
        sdtx_pos(panel_x / cell, (panel_y + 32.0f + (float)rank * 12.0f) / cell);
        sdtx_printf("%-16s %3d %3d %5s",
                    label,
                    g.scoreboard.rows[idx].kills,
                    g.scoreboard.rows[idx].deaths,
                    kdr);
    }
}

static void hud_draw_shared_panels(float screen_w, float screen_h, float sig_quality, bool compact) {
    hud_draw_alpha_banner_and_connection(screen_w, compact);
    hud_draw_nav_label(screen_w, screen_h);
    hud_draw_hail_sigil(screen_w, screen_h);
    hud_draw_module_inspect_pane(screen_w);
    hud_draw_inspect_snapshot_pane(screen_w, screen_h);
    hud_draw_signal_lost_warning(screen_w, screen_h, sig_quality);
    hud_draw_hit_indicator(screen_w, screen_h);
    hud_draw_kill_feed(screen_w, screen_h);
    hud_draw_kill_confirm(screen_w, screen_h);
    hud_draw_kill_counter(screen_w);
    hud_draw_scoreboard(screen_w, screen_h);
    /* Hit feedback vignette — drawn last so it sits above the HUD
     * readouts, but the inset rectangle in the middle leaves the
     * action row + flight readouts unobscured. */
    draw_damage_flash(screen_w, screen_h);
}

/* Station-local balances, broken out per station. Reports the station holding
 * the largest player-owned balance plus how many *other* stations also have a
 * positive balance, so the HUD can show local-credit spread without flattening
 * it into one global number. Network authority reads only the recipient-scoped
 * known-ledger snapshot; offline tools fall back to the mirrored ledger. */
static bool client_ledger_balance_summary(int *strongest_idx,
                                          float *strongest_balance,
                                          int *other_count) {
    if (strongest_idx) *strongest_idx = -1;
    if (strongest_balance) *strongest_balance = 0.0f;
    if (other_count) *other_count = 0;
    if (g.net_authority_enabled) {
        int best_idx = -1;
        float best_bal = 0.0f;
        int positives = 0;
        for (int i = 0; i < g.known_station_ledger_count; i++) {
            const NetKnownLedgerEntry *entry = &g.known_station_ledger[i];
            if (entry->station >= MAX_STATIONS || entry->balance <= 0.5f)
                continue;
            positives++;
            if (entry->balance > best_bal) {
                best_bal = entry->balance;
                best_idx = (int)entry->station;
            }
        }
        if (best_idx < 0) return false;
        if (strongest_idx) *strongest_idx = best_idx;
        if (strongest_balance) *strongest_balance = best_bal;
        if (other_count) *other_count = positives - 1;
        return true;
    }
    if (!g.local_server.active) return false;
    uint8_t pseudo[32];
    client_session_pseudo_pubkey(LOCAL_PLAYER.session_token, pseudo);
    int best_idx = -1;
    float best_bal = 0.0f;
    int positives = 0;
    for (int si = 0; si < MAX_STATIONS; si++) {
        const station_t *st = &g.world.stations[si];
        float bal = 0.0f;
        for (int li = 0; li < st->ledger_count; li++) {
            if (memcmp(st->ledger[li].player_pubkey,
                       pseudo, 32) == 0) {
                bal += st->ledger[li].balance;
            }
        }
        if (bal > 0.5f) {
            positives++;
            if (bal > best_bal) {
                best_bal = bal;
                best_idx = si;
            }
        }
    }
    if (best_idx < 0) return false;
    if (strongest_idx) *strongest_idx = best_idx;
    if (strongest_balance) *strongest_balance = best_bal;
    if (other_count) *other_count = positives - 1;
    return true;
}

/* Currency label at the player's current (docked or nearby) station.
 * Falls back to "cr" when the station hasn't set a currency_name. */
static const char *player_current_currency(void) {
    int st = LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station;
    if (st < 0 || st >= MAX_STATIONS) return "cr";
    const char *cn = g.world.stations[st].currency_name;
    return (cn && cn[0]) ? cn : "cr";
}

/* ------------------------------------------------------------------ */
/* UI scaling / layout helpers                                         */
/* ------------------------------------------------------------------ */

static const float UI_SCALE_TIGHT   = 1.45f;
static const float UI_SCALE_COMPACT = 1.60f;
static const float UI_SCALE_DEFAULT = 1.42f;
static const float UI_SCALE_WIDE    = 1.28f;

/* ui_safe_positive is shared via client.h so render_world (and any
 * other direct sapp_* consumer) can guard against the same NaN frame. */

float ui_window_width(void) {
    float px  = ui_safe_positive(sapp_widthf(), 1280.0f);
    float dpi = ui_safe_positive(sapp_dpi_scale(), 1.0f);
    return px / fmaxf(1.0f, dpi);
}

float ui_window_height(void) {
    float px  = ui_safe_positive(sapp_heightf(), 720.0f);
    float dpi = ui_safe_positive(sapp_dpi_scale(), 1.0f);
    return px / fmaxf(1.0f, dpi);
}

float ui_scale(void) {
    float width = ui_window_width();
    float height = ui_window_height();
    if ((width < 900.0f) || (height < 620.0f)) {
        return UI_SCALE_TIGHT;
    }
    if ((width < 1280.0f) || (height < 780.0f)) {
        return UI_SCALE_COMPACT;
    }
    if ((width > 1800.0f) && (height > 980.0f)) {
        return UI_SCALE_WIDE;
    }
    return UI_SCALE_DEFAULT;
}

float ui_screen_width(void) {
    return ui_window_width() / ui_scale();
}

float ui_screen_height(void) {
    return ui_window_height() / ui_scale();
}

bool ui_is_compact(void) {
    return (ui_window_width() < 1200.0f) || (ui_window_height() < 760.0f);
}

float ui_text_zoom(void) {
    return 1.0f;
}

float ui_text_pos(float pixel_value) {
    /* Snap to the debugtext cell grid so scaled layouts don't self-overlap. */
    return roundf(pixel_value / (HUD_CELL * ui_text_zoom()));
}

/* ------------------------------------------------------------------ */
/* UI drawing primitives                                               */
/* ------------------------------------------------------------------ */

void draw_ui_scanlines(float x, float y, float width, float height, float spacing, float alpha) {
    begin_line_batch();
    for (float scan_y = y + 10.0f; scan_y < (y + height - 10.0f); scan_y += spacing) {
        draw_segment_batched(v2(x + 10.0f, scan_y), v2(x + width - 10.0f, scan_y), 0.08f, 0.14f, 0.20f, alpha);
    }
    end_line_batch();
}

void draw_ui_corner_brackets(float x, float y, float width, float height, float r, float g0, float b, float alpha) {
    float arm = fminf(26.0f, fminf(width, height) * 0.16f);
    float inset = 3.0f;
    begin_line_batch();
    draw_segment_batched(v2(x + inset, y + arm), v2(x + inset, y + inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + inset, y + inset), v2(x + arm, y + inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - arm, y + inset), v2(x + width - inset, y + inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - inset, y + inset), v2(x + width - inset, y + arm), r, g0, b, alpha);
    draw_segment_batched(v2(x + inset, y + height - arm), v2(x + inset, y + height - inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + inset, y + height - inset), v2(x + arm, y + height - inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - arm, y + height - inset), v2(x + width - inset, y + height - inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - inset, y + height - inset), v2(x + width - inset, y + height - arm), r, g0, b, alpha);
    end_line_batch();
}

void draw_ui_rule(float x0, float x1, float y, float r, float g0, float b, float alpha) {
    draw_segment(v2(x0, y), v2(x1, y), r, g0, b, alpha);
}

void draw_ui_panel(float x, float y, float width, float height, float accent) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    float accent_r = 0.26f + (accent * 0.28f);
    float accent_g = 0.72f + (accent * 0.20f);
    float accent_b = 0.98f;

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.015f, 0.028f, 0.05f, 0.94f);
    draw_rect_centered(center, (width * 0.5f) - 2.0f, (height * 0.5f) - 2.0f, 0.018f, 0.044f, 0.072f, 0.86f);
    draw_ui_scanlines(x, y, width, height, 8.0f, 0.06f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.09f, 0.18f, 0.28f, 0.34f);
    draw_ui_corner_brackets(x, y, width, height, accent_r, accent_g, accent_b, 0.82f);
    draw_ui_rule(x + 14.0f, x + fminf(116.0f, width * 0.28f), y + 14.0f, accent_r, accent_g, accent_b, 0.82f);
    draw_ui_rule(x + width - fminf(56.0f, width * 0.18f), x + width - 14.0f, y + 14.0f, 0.18f, 0.28f, 0.38f, 0.55f);
}

void draw_ui_panel_colored(float x, float y, float width, float height, float ar, float ag, float ab) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.015f, 0.028f, 0.05f, 0.94f);
    draw_rect_centered(center, (width * 0.5f) - 2.0f, (height * 0.5f) - 2.0f, 0.018f, 0.044f, 0.072f, 0.86f);
    draw_ui_scanlines(x, y, width, height, 8.0f, 0.06f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.09f, 0.18f, 0.28f, 0.34f);
    draw_ui_corner_brackets(x, y, width, height, ar, ag, ab, 0.82f);
    draw_ui_rule(x + 14.0f, x + fminf(116.0f, width * 0.28f), y + 14.0f, ar, ag, ab, 0.82f);
    draw_ui_rule(x + width - fminf(56.0f, width * 0.18f), x + width - 14.0f, y + 14.0f, 0.18f, 0.28f, 0.38f, 0.55f);
}

void get_station_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    bool cramped = compact && (screen_w < 280.0f || screen_h < 300.0f);
    float hud_margin = compact ? (cramped ? 10.0f : 14.0f) : HUD_MARGIN;
    float top_reserved = compact ? (cramped ? 18.0f : 38.0f)
                                 : (HUD_TOP_PANEL_HEIGHT + 16.0f);
    float bottom_reserved = compact ? (cramped ? 8.0f : 14.0f)
                                    : (HUD_BOTTOM_PANEL_HEIGHT + 14.0f);
    float panel_width = fminf(compact ? STATION_PANEL_COMPACT_WIDTH : STATION_PANEL_WIDTH,
                              screen_w - (hud_margin * 2.0f));
    float available_h = screen_h - (hud_margin * 2.0f) -
                        top_reserved - bottom_reserved;
    if (available_h < 96.0f)
        available_h = fmaxf(64.0f, screen_h - (hud_margin * 2.0f));
    float panel_height = fminf(compact ? STATION_PANEL_COMPACT_HEIGHT : STATION_PANEL_HEIGHT,
                               available_h);
    float panel_x = (screen_w - panel_width) * 0.5f;
    float min_y = hud_margin + top_reserved;
    float max_y = screen_h - hud_margin - bottom_reserved - panel_height;
    float panel_y = clampf((screen_h - panel_height) * 0.5f, min_y, fmaxf(min_y, max_y));
    if (panel_y + panel_height > screen_h - hud_margin)
        panel_y = fmaxf(hud_margin, screen_h - hud_margin - panel_height);

    *x = panel_x;
    *y = panel_y;
    *width = panel_width;
    *height = panel_height;
}

void draw_ui_scrim(float alpha) {
    draw_rect_centered(v2(ui_screen_width() * 0.5f, ui_screen_height() * 0.5f), ui_screen_width() * 0.5f, ui_screen_height() * 0.5f, 0.01f, 0.03f, 0.06f, alpha);
}

void draw_ui_meter(float x, float y, float width, float height, float fill, float r, float g0, float b) {
    float clamped_fill = clampf(fill, 0.0f, 1.0f);
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.03f, 0.06f, 0.10f, 0.98f);
    draw_ui_scanlines(x, y, width, height, 4.0f, 0.05f);
    if (clamped_fill > 0.0f) {
        float fill_width = width * clamped_fill;
        draw_rect_centered(v2(x + (fill_width * 0.5f), y + (height * 0.5f)), fill_width * 0.5f, height * 0.5f, r, g0, b, 0.92f);
        draw_ui_rule(x + 2.0f, x + fill_width - 2.0f, y + 2.0f, fminf(1.0f, r + 0.18f), fminf(1.0f, g0 + 0.18f), fminf(1.0f, b + 0.18f), 0.72f);
    }
    draw_ui_corner_brackets(x, y, width, height, 0.24f, 0.48f, 0.62f, 0.70f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.12f, 0.22f, 0.32f, 0.48f);
}

/* ------------------------------------------------------------------ */
/* HUD layout rects                                                    */
/* ------------------------------------------------------------------ */

void get_flight_hud_rects(float* top_x, float* top_y, float* top_w, float* top_h,
    float* bottom_x, float* bottom_y, float* bottom_w, float* bottom_h) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float top_width = fminf(compact ? HUD_TOP_PANEL_COMPACT_WIDTH : HUD_TOP_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float top_height = compact ? HUD_TOP_PANEL_COMPACT_HEIGHT : HUD_TOP_PANEL_HEIGHT;
    float bottom_width = fminf(compact ? HUD_BOTTOM_PANEL_COMPACT_WIDTH : HUD_BOTTOM_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float bottom_height = compact ? 28.0f : HUD_BOTTOM_PANEL_HEIGHT;

    *top_x = hud_margin;
    *top_y = hud_margin;
    *top_w = top_width;
    *top_h = top_height;
    *bottom_x = hud_margin;
    *bottom_y = screen_h - hud_margin - bottom_height;
    *bottom_w = bottom_width;
    *bottom_h = bottom_height;
}

bool hud_should_draw_message_panel(void) {
    if (episode_is_active(&g.episode)) return false;
    return !LOCAL_PLAYER.docked || !g.onboarding.complete || !g.onboarding.welcomed ||
           (g.notice_timer > 0.0f) || (g.collection_feedback_timer > 0.0f);
}

void get_hud_message_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = ui_screen_width();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float panel_w = compact ? HUD_MESSAGE_PANEL_COMPACT_WIDTH : HUD_MESSAGE_PANEL_WIDTH;
    float panel_h = compact ? HUD_MESSAGE_PANEL_COMPACT_HEIGHT : HUD_MESSAGE_PANEL_HEIGHT;
    float gap = compact ? 8.0f : 12.0f;

    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);
    panel_w = fminf(panel_w, screen_w - (hud_margin * 2.0f));

    *x = screen_w - hud_margin - panel_w;
    *y = bottom_y - gap - panel_h;
    *width = panel_w;
    *height = panel_h;
}

/* ------------------------------------------------------------------ */
/* Message line splitting / building                                   */
/* ------------------------------------------------------------------ */

/* Word-wrap `text` into up to `max_lines` output slots of size
 * `line_cap` each. Each line is at most `max_cols` characters; line
 * breaks are preferred at spaces. If the text doesn't fit in the
 * available slots, the last slot is truncated with "...". Slots past
 * what the text needs are left as empty strings. */
static void wrap_hud_message_lines(const char *text, int max_cols,
                                   char (*lines)[96], int line_cap,
                                   int max_lines) {
    for (int i = 0; i < max_lines; i++) lines[i][0] = '\0';
    if (!text || !text[0] || max_lines <= 0) return;
    if (max_cols < 8) max_cols = 8;

    int li = 0;
    const char *cursor = text;
    while (*cursor && li < max_lines) {
        size_t remaining = strlen(cursor);
        if ((int)remaining <= max_cols) {
            snprintf(lines[li], line_cap, "%.*s",
                     (int)(line_cap - 1), cursor);
            li++;
            break;
        }
        int split = max_cols;
        while (split > (max_cols / 2) && cursor[split] != ' ') split--;
        if (split <= max_cols / 2) split = max_cols; /* no space → hard cut */
        /* Last slot and still more text after the split — append "..." */
        if (li == max_lines - 1) {
            int tail = split - 3 < 0 ? 0 : split - 3;
            snprintf(lines[li], line_cap, "%.*s...", tail, cursor);
            li++;
            break;
        }
        snprintf(lines[li], line_cap, "%.*s", split, cursor);
        li++;
        cursor += split;
        while (*cursor == ' ') cursor++;
    }
}

typedef struct {
    int station_idx;
    station_construction_need_t need;
    int held;
} hud_construction_need_t;

typedef struct {
    int station_idx;
    int ring;
    int slot;
    module_type_t type;
} hud_snapping_scaffold_t;

typedef struct {
    int station_idx;
    int blocker_idx;
    module_type_t type;
} hud_shipyard_blocked_t;

typedef struct {
    int station_idx;
} hud_abandoned_plan_t;

static const char *hud_station_short_name(int station_idx) {
    const char *name = station_short_name(station_idx);
    return (name && name[0] != '?') ? name : "station";
}

static int hud_ship_material_count(commodity_t material) {
    if ((unsigned)material >= COMMODITY_COUNT) return 0;
    if (material >= COMMODITY_RAW_ORE_COUNT)
        return manifest_count_by_commodity(&LOCAL_PLAYER.ship->manifest, material);
    return (int)floorf(LOCAL_PLAYER.ship->cargo[material] + 0.001f);
}

static const char *hud_material_source_hint(commodity_t material) {
    switch (material) {
    case COMMODITY_FRAME:
        return "Make frames at a Frame Press, then dock and press [S].";
    case COMMODITY_FERRITE_INGOT:
    case COMMODITY_CUPRITE_INGOT:
    case COMMODITY_CRYSTAL_INGOT:
        return "Smelt matching ore at a Furnace, then dock and press [S].";
    case COMMODITY_LASER_MODULE:
    case COMMODITY_TRACTOR_MODULE:
        return "Fabricate modules at a fab, then dock and press [S].";
    default:
        return "Haul the material here, then dock and press [S].";
    }
}

static bool hud_station_construction_need(const station_t *st, int station_idx,
                                          hud_construction_need_t *out) {
    if (!st || !out) return false;
    station_construction_need_t need;
    if (!station_construction_material_need(st, &need)) return false;
    *out = (hud_construction_need_t){
        .station_idx = station_idx,
        .need = need,
        .held = hud_ship_material_count(need.material),
    };
    return true;
}

static bool hud_find_construction_need(hud_construction_need_t *out) {
    if (!out) return false;
    int focus = -1;
    if (LOCAL_PLAYER.docked) focus = LOCAL_PLAYER.current_station;
    else if (LOCAL_PLAYER.in_dock_range) focus = LOCAL_PLAYER.nearby_station;
    if (focus >= 0 && focus < MAX_STATIONS &&
        hud_station_construction_need(&g.world.stations[focus], focus, out)) {
        return true;
    }

    const float max_range = 1400.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_construction_need_t best = {0};
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        float d2 = v2_dist_sq(st->pos, LOCAL_PLAYER.ship->pos);
        if (d2 >= best_d2) continue;
        hud_construction_need_t candidate;
        if (!hud_station_construction_need(st, s, &candidate)) continue;
        best = candidate;
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}

static bool hud_station_shipyard_blocked(const station_t *st, int station_idx,
                                         hud_shipyard_blocked_t *out) {
    if (!st || !out) return false;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    if (st->pending_scaffold_count <= 0) return false;
    if (!station_has_module(st, MODULE_SHIPYARD)) return false;
    if (station_nascent_scaffold_index(g.world.scaffolds, MAX_SCAFFOLDS,
                                       station_idx) >= 0) {
        return false;
    }
    int blocker = station_construction_blocker_index(st, g.world.scaffolds,
                                                     MAX_SCAFFOLDS);
    if (blocker < 0) return false;

    *out = (hud_shipyard_blocked_t){
        .station_idx = station_idx,
        .blocker_idx = blocker,
        .type = st->pending_scaffolds[0].type,
    };
    return true;
}

static bool hud_find_shipyard_blocked(hud_shipyard_blocked_t *out) {
    if (!out) return false;
    int focus = -1;
    if (LOCAL_PLAYER.docked) focus = LOCAL_PLAYER.current_station;
    else if (LOCAL_PLAYER.in_dock_range) focus = LOCAL_PLAYER.nearby_station;
    if (focus >= 0 && focus < MAX_STATIONS &&
        hud_station_shipyard_blocked(&g.world.stations[focus], focus, out)) {
        return true;
    }

    const float max_range = 1400.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_shipyard_blocked_t best = {0};
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        float d2 = v2_dist_sq(st->pos, LOCAL_PLAYER.ship->pos);
        if (d2 >= best_d2) continue;
        hud_shipyard_blocked_t candidate;
        if (!hud_station_shipyard_blocked(st, s, &candidate)) continue;
        best = candidate;
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}

static bool hud_find_abandoned_plan(hud_abandoned_plan_t *out) {
    if (!out) return false;
    const float max_range = 1800.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_abandoned_plan_t best = {0};
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_planned_site_abandoned(st)) continue;
        float d2 = v2_dist_sq(st->pos, LOCAL_PLAYER.ship->pos);
        if (d2 >= best_d2) continue;
        best.station_idx = s;
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}

static bool hud_find_snapping_scaffold(hud_snapping_scaffold_t *out) {
    if (!out) return false;
    const float max_range = 900.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_snapping_scaffold_t best = {0};
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &g.world.scaffolds[i];
        if (!sc->active || sc->state != SCAFFOLD_SNAPPING) continue;
        if (sc->placed_station < 0 || sc->placed_station >= MAX_STATIONS) continue;
        float d2 = v2_dist_sq(sc->pos, LOCAL_PLAYER.ship->pos);
        if (d2 >= best_d2) continue;
        best = (hud_snapping_scaffold_t){
            .station_idx = sc->placed_station,
            .ring = sc->placed_ring,
            .slot = sc->placed_slot,
            .type = sc->module_type,
        };
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}


static bool build_hud_message(char* label, size_t label_size, char* message, size_t message_size, uint8_t* r, uint8_t* g0, uint8_t* b) {
    /* ================================================================
     * SYSTEM HINT PANEL — bottom-right, one message at a time.
     * Tutorial copy uses the local guide voice; stations never speak
     * here because they use the hail overlay.
     * ================================================================ */

    /* Subtitle-style messages: one clean line, no label brackets.
     * Priority order — only the most important message shows. */

    /* Hull critical */
    if (!LOCAL_PLAYER.docked && g.death_screen_timer <= 0.0f) {
        float max_hull = ship_max_hull(LOCAL_PLAYER.ship);
        if (max_hull > 0.0f && LOCAL_PLAYER.ship->hull / max_hull < 0.20f) {
            label[0] = '\0';
            snprintf(message, message_size, "Hull failing. Dock for repairs.");
            *r = 255; *g0 = 60; *b = 50;
            return true;
        }
    }

    /* Transient notice */
    if (g.notice_timer > 0.0f) {
        label[0] = '\0';
        snprintf(message, message_size, "%s", g.notice);
        *r = 140; *g0 = 140; *b = 140;
        return true;
    }

    /* Plan mode */
    if (g.plan_mode_active) {
        bool ghost = (g.plan_target_station == -1);
        snprintf(label, label_size, "%s", ghost ? "PLAN GHOST" : "PLAN SLOT");
        if (g.placement_target_ring > 0 && g.placement_target_slot >= 0) {
            if (ghost) {
                snprintf(message, message_size,
                         "%s preview ring %d slot %d. [E] locks an outpost; [R] changes type; [B] exits.",
                         module_type_name((module_type_t)g.plan_type),
                         g.placement_target_ring,
                         g.placement_target_slot);
            } else {
                snprintf(message, message_size,
                         "%s at %s ring %d slot %d. [E] toggles this slot; [R] changes type; [B] exits.",
                         module_type_name((module_type_t)g.plan_type),
                         hud_station_short_name(g.placement_target_station),
                         g.placement_target_ring,
                         g.placement_target_slot);
                if (g.placement_target_station >= 0 &&
                    g.placement_target_station < MAX_STATIONS) {
                    const station_t *st = &g.world.stations[g.placement_target_station];
                    station_plan_flow_hint_t hint;
                    char flow_hint[96];
                    if (station_plan_flow_hint(st, (module_type_t)g.plan_type,
                                               g.placement_target_ring,
                                               g.placement_target_slot,
                                               &hint) &&
                        station_plan_flow_hint_format(&hint, flow_hint,
                                                      sizeof(flow_hint))) {
                        size_t used = strlen(message);
                        if (used + 1 < message_size)
                            snprintf(message + used, message_size - used,
                                     " %s.", flow_hint);
                    }
                }
            }
        } else {
            snprintf(message, message_size,
                     "%s planning. [R] changes type; [E] places; [B] exits.",
                     module_type_name((module_type_t)g.plan_type));
        }
        if (ghost) { *r = 180; *g0 = 165; *b = 115; }
        else       { *r = 130; *g0 = 190; *b = 210; }
        return true;
    }

    /* Autopilot */
    if (LOCAL_PLAYER.autopilot_mode) {
        label[0] = '\0';
        snprintf(message, message_size, "Autopilot active. Move to cancel.");
        *r = 180; *g0 = 160; *b = 90;
        return true;
    }

    /* Scaffold tow */
    if (LOCAL_PLAYER.ship->towed_scaffold >= 0) {
        snprintf(label, label_size, "SCAFFOLD TOW");
        snprintf(message, message_size, "Towing scaffold. [E] place, tap [Space] release.");
        *r = 160; *g0 = 150; *b = 100;
        return true;
    }

    hud_snapping_scaffold_t snap;
    if (hud_find_snapping_scaffold(&snap)) {
        snprintf(label, label_size, "SCAFFOLD SNAP");
        snprintf(message, message_size,
                 "%s snapping to %s ring %d slot %d. Supply starts when it locks.",
                 module_type_name(snap.type),
                 hud_station_short_name(snap.station_idx),
                 snap.ring,
                 snap.slot);
        *r = 190; *g0 = 170; *b = 100;
        return true;
    }

    hud_shipyard_blocked_t blocked;
    if (hud_find_shipyard_blocked(&blocked)) {
        snprintf(label, label_size, "YARD BLOCKED");
        snprintf(message, message_size,
                 "%s yard blocked by loose scaffold. Tow it clear to start %s.",
                 hud_station_short_name(blocked.station_idx),
                 module_type_name(blocked.type));
        *r = 255; *g0 = 140; *b = 60;
        return true;
    }

    hud_abandoned_plan_t abandoned;
    if (hud_find_abandoned_plan(&abandoned)) {
        snprintf(label, label_size, "ABANDONED PLAN");
        snprintf(message, message_size,
                 "%s has no reserved modules. Enter plan mode to add one, or clear the blueprint.",
                 hud_station_short_name(abandoned.station_idx));
        *r = 210; *g0 = 115; *b = 75;
        return true;
    }

    hud_construction_need_t need;
    if (hud_find_construction_need(&need)) {
        snprintf(label, label_size, "SUPPLY NEED");
        const char *station = hud_station_short_name(need.station_idx);
        const char *mat = commodity_short_label(need.need.material);
        int needed = (int)ceilf(need.need.remaining - 0.001f);
        if (needed < 1) needed = 1;
        if (need.held > 0) {
            snprintf(message, message_size,
                     "%s needs %d %s at %s. You carry %d; dock and press [S].",
                     need.need.station_shell ? "Outpost scaffold" : module_type_name(need.need.module_type),
                     needed, mat, station, need.held);
        } else {
            snprintf(message, message_size,
                     "%s needs %d %s at %s. %s",
                     need.need.station_shell ? "Outpost scaffold" : module_type_name(need.need.module_type),
                     needed, mat, station,
                     hud_material_source_hint(need.need.material));
        }
        *r = 205; *g0 = 165; *b = 90;
        return true;
    }

    /* Onboarding */
    if (onboarding_hint(label, label_size, message, message_size)) {
        *r = 255; *g0 = 221; *b = 119;
        return true;
    }

    /* Hold full */
    {
        int cargo = (int)lroundf(ship_total_cargo(LOCAL_PLAYER.ship));
        int cap = (int)lroundf(ship_cargo_capacity(LOCAL_PLAYER.ship));
        if (cargo >= cap) {
            label[0] = '\0';
            snprintf(message, message_size, "Hold full. Dock to sell.");
            *r = 180; *g0 = 150; *b = 80;
            return true;
        }
    }

    /* Docking */
    if (LOCAL_PLAYER.docking_approach) {
        label[0] = '\0';
        snprintf(message, message_size, "Docking. W or S to cancel.");
        *r = 120; *g0 = 160; *b = 150;
        return true;
    }
    if (LOCAL_PLAYER.in_dock_range) {
        label[0] = '\0';
        snprintf(message, message_size, "Press E to dock.");
        *r = 120; *g0 = 160; *b = 150;
        return true;
    }

    /* Tractor pickup */
    if (g.collection_feedback_timer > 0.0f) {
        label[0] = '\0';
        snprintf(message, message_size, "+%d fragment%s collected.",
            g.collection_feedback_fragments,
            g.collection_feedback_fragments == 1 ? "" : "s");
        mining_grade_rgb((mining_grade_t)hud_best_towed_fragment_grade(),
                         r, g0, b);
        return true;
    }

    /* Nothing to say. Panel is empty. */
    return false;
}

#ifdef __EMSCRIPTEN__
static int smoke_loop_state_override = 0;
static int smoke_apply_loop_state(int state);
enum { SMOKE_OUTPOST_INDEX = SIGNAL_FIRST_OUTPOST_INDEX };

static bool smoke_hooks_enabled(void) {
    return emscripten_run_script_int(
        "(new URLSearchParams(location.search).get('smoke')==='1')") != 0;
}

EMSCRIPTEN_KEEPALIVE
const char *get_hud_hint_text(void) {
    static char out[384];
    char label[64];
    char message[256];
    uint8_t r = 0, g0 = 0, b = 0;

    if (smoke_loop_state_override != 0)
        (void)smoke_apply_loop_state(smoke_loop_state_override);

    out[0] = '\0';
    label[0] = '\0';
    message[0] = '\0';
    if (!build_hud_message(label, sizeof(label), message, sizeof(message), &r, &g0, &b))
        return out;

    if (label[0] != '\0')
        snprintf(out, sizeof(out), "%s :: %s", label, message);
    else
        snprintf(out, sizeof(out), "%s", message);
    return out;
}

EMSCRIPTEN_KEEPALIVE
const char *get_hud_action_text(void) {
    static char out[192];
    int cargo_units = (int)lroundf(ship_total_cargo(LOCAL_PLAYER.ship));
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(LOCAL_PLAYER.ship));
    float sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
    hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);

    if (smoke_loop_state_override != 0) {
        (void)smoke_apply_loop_state(smoke_loop_state_override);
        cargo_units = (int)lroundf(ship_total_cargo(LOCAL_PLAYER.ship));
        cargo_capacity = (int)lroundf(ship_cargo_capacity(LOCAL_PLAYER.ship));
        sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
        act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
    }

    out[0] = '\0';
    hud_format_action_wide(&act, current_station_ptr(), out, sizeof(out));
    return out;
}

enum {
    SMOKE_LOOP_STATE_CLEAR = 0,
    SMOKE_LOOP_STATE_FRAGMENTS_NEARBY = 1,
    SMOKE_LOOP_STATE_TRACTOR_REACHING = 2,
    SMOKE_LOOP_STATE_TRACTOR_LOCK = 3,
    SMOKE_LOOP_STATE_TOWING = 4,
    SMOKE_LOOP_STATE_HAIL_READY = 5,
    SMOKE_LOOP_STATE_HAIL_NOTICE = 6,
    SMOKE_LOOP_STATE_PLAN_GHOST = 7,
    SMOKE_LOOP_STATE_PLAN_SLOT = 8,
    SMOKE_LOOP_STATE_SCAFFOLD_SNAP = 9,
    SMOKE_LOOP_STATE_SUPPLY_NEED = 10,
    SMOKE_LOOP_STATE_YARD_BLOCKED = 11,
    SMOKE_LOOP_STATE_ABANDONED_PLAN = 12,
    SMOKE_LOOP_STATE_FRACTURE_TABLEAU = 13,
    SMOKE_LOOP_STATE_REMOTE_PILOT_SCAN = 14,
    SMOKE_LOOP_STATE_WEAK_SIGNAL_VISUAL = 15,
    SMOKE_LOOP_STATE_NARROW_CAMERA_OFFSET = 16,
    SMOKE_LOOP_STATE_CUPRITE_GATE = 17,
    SMOKE_LOOP_STATE_SCAN_LASER_FAB = 18,
    SMOKE_LOOP_STATE_TRACKED_CUPRITE_CONTRACT = 19,
    SMOKE_LOOP_STATE_ONBOARDING_DELIVER = 20,
    SMOKE_LOOP_STATE_ONBOARDING_RETURN = 21,
    SMOKE_LOOP_STATE_ONBOARDING_MARKET = 22,
    SMOKE_LOOP_STATE_ONBOARDING_COMPLETE = 23,
    SMOKE_LOOP_STATE_CARGO_TOWING = 24,
    SMOKE_LOOP_STATE_MODULE_CARGO_TRACTOR = 25,
    SMOKE_LOOP_STATE_ROCK_SMELT_PATH = 26,
    SMOKE_LOOP_STATE_ROCK_ROUTE_TARGET = 27,
    SMOKE_LOOP_STATE_ROCK_ROUTE_TOW = 28,
    SMOKE_LOOP_STATE_ROCK_ROUTE_DEGRADED = 29,
    SMOKE_LOOP_STATE_CARGO_LINEAGE = 30,
    SMOKE_LOOP_STATE_LOCAL_MONEY = 31,
    SMOKE_LOOP_STATE_NPC_MOTIVE_CRISP = 32,
    SMOKE_LOOP_STATE_NPC_MOTIVE_DEGRADED = 33,
    SMOKE_LOOP_STATE_REMEMBERED_WORK_CRISP = 34,
    SMOKE_LOOP_STATE_REMEMBERED_WORK_DEGRADED = 35,
    SMOKE_LOOP_STATE_CONSTRUCTION_CONSEQUENCE = 36,
    SMOKE_LOOP_STATE_STATION_FRAGMENT_TRACTOR = 37,
};

static int smoke_remembered_work_mode = -1;
static bool smoke_construction_snapshot_valid = false;
static int smoke_construction_snapshot_indices[2] = {-1, -1};
static station_module_t smoke_construction_snapshot_modules[2];

static bool smoke_maintain_npc_motive_view(bool degraded) {
    if (g.world.station_count <= 1 ||
        !station_exists(&g.world.stations[0]) ||
        !station_exists(&g.world.stations[1])) return false;

    server_player_t *sp = &LOCAL_PLAYER;
    npc_ship_t *npc = &g.world.npc_ships[0];
    npc->active = true;
    npc->role = NPC_ROLE_HAULER;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->home_station = 0;
    npc->dest_station = 1;
    const uint8_t token[8] = {'N','P','C','0','0','7','0','1'};
    memcpy(npc->session_token, token, sizeof(token));
    if (npc->ship) {
        npc->ship->pos = v2_add(sp->ship->pos, v2(120.0f, 0.0f));
        npc->ship->vel = v2(0.0f, 0.0f);
    }

    memset(&g.inspect_snapshot, 0, sizeof(g.inspect_snapshot));
    g.inspect_snapshot.target_type = INSPECT_TARGET_NPC;
    g.inspect_snapshot.target_index = 0;
    g.inspect_snapshot.module_index = 0xffu;
    g.inspect_snapshot.role = (uint8_t)NPC_ROLE_HAULER;
    g.inspect_snapshot.state = (uint8_t)NPC_STATE_TRAVEL_TO_DEST;
    g.inspect_snapshot.home_station = 0;
    g.inspect_snapshot.dest_station = 1;
    g.inspect_snapshot.row_count = 1;

    NetInspectSnapshotRow *job = &g.inspect_snapshot.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->grade = degraded ? 90u : 245u;
    job->chain_len = 255u;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->event_id = (uint64_t)0u |
                    ((uint64_t)1u << 8) |
                    ((uint64_t)INSPECT_DIAG_JOB_HAUL << 16) |
                    ((uint64_t)COMMODITY_FERRITE_INGOT << 24);
    job->cargo_pub[INSPECT_JOB_FACTOR_VALUE] = degraded ? 72u : 190u;
    job->cargo_pub[INSPECT_JOB_FACTOR_ROUTE] = degraded ? 85u : 245u;
    job->cargo_pub[INSPECT_JOB_FACTOR_FRESHNESS] = degraded ? 58u : 230u;
    job->cargo_pub[INSPECT_JOB_META_REASON] =
        (uint8_t)INSPECT_JOB_REASON_ROUTE_MEMORY;
    job->cargo_pub[INSPECT_JOB_META_MEMORY_KIND] =
        (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    job->cargo_pub[INSPECT_JOB_META_HOPS] = degraded ? 4u : 0u;
    job->cargo_pub[INSPECT_JOB_META_AGE] = degraded ? 61u : 2u;
    job->cargo_pub[INSPECT_JOB_META_SOURCE_STATION] = 0u;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0x90 + i);

    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = -1;
    sp->in_dock_range = false;
    g.was_docked = false;
    g.inspect_snapshot_timer = 6.0f;
    g.local_server.active = false;
    return true;
}

static bool smoke_maintain_remembered_work_view(void) {
    if (g.world.station_count <= 1 ||
        !station_exists(&g.world.stations[1])) return false;
    server_player_t *sp = &LOCAL_PLAYER;
    station_t *st = &g.world.stations[1];
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->ship->pos = st->pos;
    g.was_docked = true;
    g.dock_settle_timer = 0.0f;
    g.station_view = STATION_VIEW_HISTORY;
    g.local_server.active = false;
    return true;
}

static bool smoke_seed_remembered_work(bool degraded) {
    int mode = degraded ? 1 : 0;
    if (g.local_server.world.station_count <= 1) return false;
    if (smoke_remembered_work_mode != mode) {
        chain_payload_route_history_t payload = {0};
        payload.memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
        payload.origin_station = 0;
        payload.destination_station = 1;
        payload.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
        payload.action = (uint8_t)CONTRACT_DELIVERY;
        payload.confidence = degraded ? 90u : 245u;
        payload.salience = degraded ? 85u : 235u;
        payload.evidence_count = degraded ? 2u : 6u;
        payload.value_hint = degraded ? 40u : 120u;
        payload.observed_tick = (uint32_t)g.world.tick;
        payload.subject_nonce = degraded
            ? UINT64_C(0x6060dd01) : UINT64_C(0x6060cc01);
        station_t *authority = &g.local_server.world.stations[1];
        if (chain_log_emit(&g.local_server.world, authority,
                           CHAIN_EVT_ROUTE_HISTORY,
                           &payload, sizeof(payload)) == 0) {
            return false;
        }
        smoke_remembered_work_mode = mode;
    }
    return smoke_maintain_remembered_work_view();
}

static bool smoke_maintain_construction_consequence_view(void) {
    if (g.world.station_count <= 0 ||
        !station_exists(&g.world.stations[0])) return false;
    server_player_t *sp = &LOCAL_PLAYER;
    station_t *st = &g.world.stations[0];
    int relay_idx = -1;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_SIGNAL_RELAY) {
            relay_idx = i;
            break;
        }
    }
    if (relay_idx < 0) return false;
    station_module_t *relay = &st->modules[relay_idx];
    relay->scaffold = false;
    relay->build_progress = 1.0f;

    /* Keep the perception-review tableau mixed: one module is still
     * stocking amber members, one is actively welding finished material,
     * and the relay is complete. This exercises all three structural states
     * at desktop and narrow viewport sizes without inventing render-only
     * progress semantics. */
    if (!smoke_construction_snapshot_valid) {
        int staged = 0;
        for (int i = 0; i < st->module_count && staged < 2; i++) {
            if (i == relay_idx) continue;
            smoke_construction_snapshot_indices[staged] = i;
            smoke_construction_snapshot_modules[staged] = st->modules[i];
            staged++;
        }
        smoke_construction_snapshot_valid = staged == 2;
    }
    if (smoke_construction_snapshot_valid) {
        for (int staged = 0; staged < 2; staged++) {
            int i = smoke_construction_snapshot_indices[staged];
            if (i < 0 || i >= st->module_count) continue;
            st->modules[i].scaffold = true;
            st->modules[i].build_progress = staged == 0 ? 0.55f : 1.55f;
        }
    }
    if (st->signal_range <= 0.0f) st->signal_range = 1600.0f;

    g.commission_timer = 1.5f;
    g.commission_pos = module_world_pos_ring(st, relay->ring, relay->slot);
    module_color_fn(MODULE_SIGNAL_RELAY,
                    &g.commission_cr, &g.commission_cg, &g.commission_cb);
    snprintf(g.notice, sizeof(g.notice), "%s",
             module_consequence_label(MODULE_SIGNAL_RELAY));
    g.notice_timer = 6.0f;
    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = 0;
    sp->in_dock_range = false;
    sp->ship->pos = v2_add(g.commission_pos, v2(180.0f, 0.0f));
    g.camera_pos = g.commission_pos;
    g.camera_initialized = true;
    g.camera_drift_timer = 0.0f;
    g.camera_station_index = -1;
    g.was_docked = false;
    g.local_server.active = false;
    return true;
}

static bool smoke_maintain_local_money_view(void) {
    if (g.world.station_count <= 1 ||
        !station_exists(&g.world.stations[0]) ||
        !station_exists(&g.world.stations[1])) return false;

    server_player_t *sp = &LOCAL_PLAYER;
    station_t *earned_here = &g.world.stations[0];
    station_t *docked_here = &g.world.stations[1];
    uint8_t pubkey[32];
    client_session_pseudo_pubkey(sp->session_token, pubkey);

    earned_here->ledger_count = 1;
    memset(&earned_here->ledger[0], 0, sizeof(earned_here->ledger[0]));
    memcpy(earned_here->ledger[0].player_pubkey, pubkey, sizeof(pubkey));
    earned_here->ledger[0].balance = 80.0f;
    earned_here->ledger[0].lifetime_credits_in = 80;
    docked_here->ledger_count = 0;
    g.known_station_ledger_count = 2;
    g.known_station_ledger[0] = (NetKnownLedgerEntry){
        .station = 0,
        .balance = 80.0f,
    };
    g.known_station_ledger[1] = (NetKnownLedgerEntry){
        .station = 1,
        .balance = 0.0f,
    };

    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->ship->pos = docked_here->pos;
    g.was_docked = true;
    g.dock_settle_timer = 0.0f;
    g.station_balance = 0.0f;
    g.station_view = STATION_VIEW_TRADE;
    g.local_server.active = false;
    return true;
}

static bool smoke_maintain_weak_signal_view(void) {
    if (g.world.station_count <= 0 ||
        !station_exists(&g.world.stations[0])) return false;

    server_player_t *sp = &LOCAL_PLAYER;
    g.episode.watched[7] = true;
    g.local_server.active = false;
    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = -1;
    sp->in_dock_range = false;
    sp->docking_approach = false;
    g.was_docked = false;
    g.dock_settle_timer = 0.0f;
    sp->ship->pos = v2(100000.0f, 100000.0f);
    sp->ship->vel = v2(0.0f, 0.0f);
    g.camera_pos = sp->ship->pos;
    g.hail_ping_origin = sp->ship->pos;
    g.hail_ping_range = (sp->ship->comm_range > 0.0f)
                      ? sp->ship->comm_range : 1500.0f;
    g.signal_visual_saturation = signal_visual_saturation(
        signal_strength_at(&g.world, sp->ship->pos));
    g.signal_visual_saturation_initialized = true;
    g.world.time = 0.5f; /* positive warning blink phase for review */
    return true;
}

static bool smoke_maintain_cargo_lineage_view(void) {
    if (g.world.station_count <= 0 ||
        !station_exists(&g.world.stations[0])) return false;
    station_t *st = &g.world.stations[0];
    int dock_module = -1;
    for (int m = 0; m < st->module_count; m++) {
        if (!st->modules[m].scaffold && st->modules[m].type == MODULE_DOCK) {
            dock_module = m;
            break;
        }
    }
    if (dock_module < 0) return false;

    uint8_t fragment_pub[32];
    for (int i = 0; i < 32; i++) fragment_pub[i] = (uint8_t)(0x31 + i);
    cargo_unit_t ingot = {0};
    cargo_unit_t frame = {0};
    if (!hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                    fragment_pub, 0, &ingot)) return false;
    ingot.origin_station = 0;
    ingot.mined_block = 4422;
    if (!hash_product(RECIPE_FRAME_BASIC, &ingot, 1, 0, &frame)) return false;
    frame.origin_station = 0;

    uint8_t legacy_seed[8] = {'L','E','G','A','C','Y','0','1'};
    cargo_unit_t legacy = {0};
    if (!hash_legacy_migrate_unit(legacy_seed, COMMODITY_FRAME, 0, &legacy))
        return false;
    legacy.origin_station = 2;

    const cargo_unit_t units[2] = {frame, legacy};
    for (int i = 0; i < 2; i++) {
        cargo_pod_t *pod = &g.world.cargo_pods[MAX_CARGO_PODS - 2 + i];
        memset(pod, 0, sizeof(*pod));
        pod->active = true;
        pod->kind = CARGO_POD_CARGO;
        pod->commodity = COMMODITY_FRAME;
        pod->quantity = 1;
        pod->manifest_count = 1;
        pod->manifest_units[0] = units[i];
        pod->pos = v2_add(module_world_pos_ring(
                              st, st->modules[dock_module].ring,
                              st->modules[dock_module].slot),
                          v2((float)i * 24.0f, 0.0f));
        pod->radius = 18.0f;
        pod->age = 2.0f;
        cargo_pod_set_module_tractor(pod, 0, dock_module);
    }
    if (st->base_price[COMMODITY_FRAME] <= FLOAT_EPSILON)
        st->base_price[COMMODITY_FRAME] = 40.0f;

    LOCAL_PLAYER.docked = true;
    LOCAL_PLAYER.current_station = 0;
    LOCAL_PLAYER.nearby_station = 0;
    LOCAL_PLAYER.in_dock_range = true;
    LOCAL_PLAYER.ship->pos = st->pos;
    g.was_docked = true;
    g.dock_settle_timer = 0.0f;
    g.station_view = STATION_VIEW_TRADE;
    g.local_server.active = false;
    return true;
}

void smoke_apply_loop_state_for_frame(void) {
    /* Most fixtures intentionally allow input and timers to evolve after
     * setup. Interaction fixtures must overwrite streamed state at the
     * render boundary to keep their visual telemetry stable. */
    if (smoke_loop_state_override ==
        SMOKE_LOOP_STATE_MODULE_CARGO_TRACTOR) {
        (void)smoke_apply_loop_state(smoke_loop_state_override);
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_STATION_FRAGMENT_TRACTOR) {
        (void)smoke_apply_loop_state(smoke_loop_state_override);
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_ROCK_SMELT_PATH) {
        (void)smoke_apply_loop_state(smoke_loop_state_override);
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_WEAK_SIGNAL_VISUAL) {
        (void)smoke_maintain_weak_signal_view();
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_CARGO_LINEAGE) {
        (void)smoke_maintain_cargo_lineage_view();
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_LOCAL_MONEY) {
        (void)smoke_maintain_local_money_view();
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_NPC_MOTIVE_CRISP) {
        (void)smoke_maintain_npc_motive_view(false);
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_NPC_MOTIVE_DEGRADED) {
        (void)smoke_maintain_npc_motive_view(true);
    } else if (smoke_loop_state_override ==
                   SMOKE_LOOP_STATE_REMEMBERED_WORK_CRISP ||
               smoke_loop_state_override ==
                   SMOKE_LOOP_STATE_REMEMBERED_WORK_DEGRADED) {
        (void)smoke_maintain_remembered_work_view();
    } else if (smoke_loop_state_override ==
               SMOKE_LOOP_STATE_CONSTRUCTION_CONSEQUENCE) {
        (void)smoke_maintain_construction_consequence_view();
    }
}

static void smoke_set_onboarding_economy_progress(bool earned,
                                                  bool docked_after_earning,
                                                  bool viewed_trade) {
    memset(&g.onboarding, 0, sizeof(g.onboarding));
    g.onboarding.loaded = true;
    g.onboarding.moved = true;
    g.onboarding.hailed = true;
    g.onboarding.fractured = true;
    g.onboarding.tractored = true;
    g.onboarding.earned = earned;
    g.onboarding.docked_after_earning = docked_after_earning;
    g.onboarding.viewed_trade = viewed_trade;
    g.onboarding.complete = earned && docked_after_earning && viewed_trade;
}

static void smoke_clear_loop_state(void) {
    server_player_t *sp = &LOCAL_PLAYER;
    float max_hull = ship_max_hull(sp->ship);

    if (smoke_construction_snapshot_valid && g.world.station_count > 0 &&
        station_exists(&g.world.stations[0])) {
        station_t *st = &g.world.stations[0];
        for (int staged = 0; staged < 2; staged++) {
            int i = smoke_construction_snapshot_indices[staged];
            if (i >= 0 && i < st->module_count)
                st->modules[i] = smoke_construction_snapshot_modules[staged];
            smoke_construction_snapshot_indices[staged] = -1;
        }
        smoke_construction_snapshot_valid = false;
    }

    g.local_server.active = true;
    g.world.interactions.count = 0;
    sp->docked = false;
    sp->in_dock_range = false;
    sp->docking_approach = false;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->hover_asteroid = -1;
    sp->scan_active = false;
    sp->scan_target_type = 0;
    sp->scan_target_index = -1;
    sp->scan_module_index = -1;
    sp->ship->towed_count = 0;
    sp->ship->towed_pod_count = 0;
    for (int i = 0; i < 10; i++)
        sp->ship->towed_pods[i] = -1;
    sp->ship->towed_scaffold = -1;
    sp->ship->tractor_active = false;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;
    sp->autopilot_mode = 0;
    if (max_hull > 0.0f)
        sp->ship->hull = max_hull;

    g.plan_mode_active = false;
    g.notice_timer = 0.0f;
    g.notice[0] = '\0';
    g.commission_timer = 0.0f;
    g.inspect_snapshot_timer = 0.0f;
    memset(&g.inspect_snapshot, 0, sizeof(g.inspect_snapshot));
    g.collection_feedback_timer = 0.0f;
    g.collection_feedback_fragments = 0;
    g.collection_feedback_ore = 0.0f;
    g.tracked_contract = -1;
    g.selected_contract = -1;
    trade_lineage_close();
    g.player_known_contract_mask = 0;
    memset(&sp->ship->knowledge, 0, sizeof(sp->ship->knowledge));
    memset(g.scanned_players, 0, sizeof(g.scanned_players));
    g.hail_timer = 0.0f;
    g.hail_station[0] = '\0';
    g.hail_message[0] = '\0';
    g.hail_credits = 0.0f;
    g.hail_station_index = -1;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        g.world.scaffolds[i].active = false;
    }
    for (int i = 0; i < 3 && i < MAX_CARGO_PODS; i++) {
        memset(&g.world.cargo_pods[MAX_CARGO_PODS - 1 - i], 0,
               sizeof(g.world.cargo_pods[MAX_CARGO_PODS - 1 - i]));
    }
    if (MAX_STATIONS > SMOKE_OUTPOST_INDEX) {
        station_t *ghost = &g.world.stations[SMOKE_OUTPOST_INDEX];
        ghost->scaffold = false;
        ghost->planned = false;
        ghost->scaffold_progress = 1.0f;
        ghost->name[0] = '\0';
        ghost->module_count = 0;
        ghost->pending_scaffold_count = 0;
    }

    if (g.world.station_count > 0 && station_exists(&g.world.stations[0]))
        sp->ship->pos = g.world.stations[0].pos;
}

static void smoke_seed_asteroid(int slot, asteroid_tier_t tier,
                                commodity_t commodity, vec2 pos,
                                float radius, float hp_ratio,
                                float seed) {
    if (slot < 0 || slot >= MAX_ASTEROIDS) return;
    asteroid_t *a = &g.world.asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = tier;
    a->commodity = commodity;
    a->pos = pos;
    a->radius = radius;
    a->max_hp = fmaxf(1.0f, radius * 4.0f);
    a->hp = a->max_hp * clampf(hp_ratio, 0.0f, 1.0f);
    a->max_ore = (tier == ASTEROID_TIER_S) ? REFINERY_INGOTS_PER_FRAGMENT : 0.0f;
    a->ore = a->max_ore;
    a->rotation = seed * 0.17f;
    a->spin = 0.0f;
    a->seed = seed;
    a->age = 2.0f;
    a->phase = ASTEROID_PHASE_SOLID;
}

static bool smoke_seed_rock_route(bool towing, bool degraded) {
    server_player_t *sp = &LOCAL_PLAYER;
    if (g.world.station_count <= 1 ||
        !station_is_active(&g.world.stations[0]) ||
        !station_is_active(&g.world.stations[1])) {
        return false;
    }

    /* Fill raw buffers so live demand cannot outrank the remembered route in
     * this presentation fixture. The furnace remains a valid lower reason. */
    for (int i = 0; i < g.world.station_count && i < MAX_STATIONS; i++)
        g.world.stations[i]._inventory_cache[COMMODITY_FERRITE_ORE] =
            REFINERY_HOPPER_CAPACITY;

    sp->hover_asteroid = towing ? -1 : 0;
    smoke_seed_asteroid(0, ASTEROID_TIER_S, COMMODITY_FERRITE_ORE,
                        v2_add(sp->ship->pos, v2(90.0f, 0.0f)),
                        12.0f, 1.0f, 212.0f);
    g.world.asteroids[0].grade = (uint8_t)MINING_GRADE_COMMON;
    if (towing) {
        sp->ship->towed_count = 1;
        sp->ship->towed_fragments[0] = 0;
    }

    market_memory_t memory = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = degraded ? 90 : 250,
        .salience = degraded ? 90 : 245,
        .quantity_hint = degraded ? 1 : 8,
        .observed_tick = g.world.tick,
        .subject_nonce = degraded ? 0x44556677u : 0x11223344u,
    };
    knowledge_view_t *view = &sp->ship->knowledge;
    view->capacity = SHIP_KNOWN_ITEM_CAP;
    view->count = 1;
    knowledge_item_t *item = &view->items[0];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = memory.confidence;
    item->salience = memory.salience;
    item->hops = degraded ? 3 : 0;
    item->observed_tick = memory.observed_tick;
    memcpy(item->payload, &memory, sizeof(memory));
    return true;
}

static int smoke_apply_loop_state(int state) {
    server_player_t *sp = &LOCAL_PLAYER;

    smoke_clear_loop_state();
    switch (state) {
    case SMOKE_LOOP_STATE_CLEAR:
        return 1;
    case SMOKE_LOOP_STATE_FRAGMENTS_NEARBY:
        sp->nearby_fragments = 3;
        return 1;
    case SMOKE_LOOP_STATE_TRACTOR_REACHING:
        sp->nearby_fragments = 4;
        sp->ship->tractor_active = true;
        return 1;
    case SMOKE_LOOP_STATE_TRACTOR_LOCK:
        sp->nearby_fragments = 5;
        sp->tractor_fragments = 2;
        sp->ship->tractor_active = true;
        return 1;
    case SMOKE_LOOP_STATE_TOWING:
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        smoke_seed_asteroid(0, ASTEROID_TIER_S, COMMODITY_FERRITE_ORE,
                            v2_add(sp->ship->pos, v2(120.0f, -30.0f)),
                            12.0f, 1.0f, 7.0f);
        g.world.asteroids[0].grade = (uint8_t)MINING_GRADE_COMMON;
        g.world.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
        sp->ship->towed_count = 1;
        sp->ship->towed_fragments[0] = 0;
        return 1;
    case SMOKE_LOOP_STATE_CARGO_TOWING: {
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        sp->ship->pos = v2_add(g.world.stations[0].pos, v2(600.0f, 0.0f));
        sp->ship->vel = v2(0.0f, 0.0f);
        g.camera_pos = sp->ship->pos;
        g.camera_initialized = true;
        int pod_idx = MAX_CARGO_PODS - 1;
        cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
        float range = fmaxf(ship_tractor_range(sp->ship), 180.0f);
        *pod = (cargo_pod_t){
            .active = true,
            .kind = CARGO_POD_CARGO,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity = 6,
            .manifest_count = 6,
            .pos = v2_add(sp->ship->pos, v2(range * 0.96f, 0.0f)),
            .vel = sp->ship->vel,
            .radius = 18.0f,
            .rotation = 0.35f,
            .age = 2.0f,
        };
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            pod->manifest_units[i] = (cargo_unit_t){
                .kind = CARGO_KIND_INGOT,
                .commodity = COMMODITY_FERRITE_INGOT,
                .grade = (uint8_t)MINING_GRADE_COMMON,
                .quantity = 1,
            };
        }
        cargo_pod_set_player_tractor(pod, sp->id);
        sp->ship->towed_pod_count = 1;
        sp->ship->towed_pods[0] = (int16_t)pod_idx;
        return 1;
    }
    case SMOKE_LOOP_STATE_MODULE_CARGO_TRACTOR: {
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        /* Freeze authoritative loopback publication while this render-only
         * fixture is active so interaction telemetry observes complete
         * frames instead of racing the next simulation clear. */
        g.local_server.active = false;
        station_t *st = &g.world.stations[0];
        int module_idx = -1;
        for (int m = 0; m < st->module_count; m++) {
            if (!st->modules[m].scaffold &&
                st->modules[m].type == MODULE_DOCK) {
                module_idx = m;
                break;
            }
        }
        if (module_idx < 0) return 0;

        station_module_t *module = &st->modules[module_idx];
        if (module->ring > 0 && module->ring <= MAX_ARMS) {
            st->arm_rotation[module->ring - 1] =
                fmodf(g.world.time * 0.35f, TWO_PI_F);
        }
        vec2 module_pos = module_world_pos_ring(
            st, module->ring, module->slot);
        vec2 outward = v2_norm(v2_sub(module_pos, st->pos));
        vec2 tangent = v2(-outward.y, outward.x);
        g.world.interactions.count = 0;
        for (int p = 0; p < 3; p++) {
            int pod_idx = MAX_CARGO_PODS - 1 - p;
            cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
            float radial = STATION_MODULE_COL_RADIUS + 190.0f + 55.0f * p;
            float lateral = ((float)p - 1.0f) * 72.0f;
            *pod = (cargo_pod_t){
                .active = true,
                .kind = CARGO_POD_CARGO,
                .commodity = COMMODITY_FERRITE_INGOT,
                .quantity = (uint16_t)(4 + p),
                .manifest_count = (uint16_t)(4 + p),
                .pos = v2_add(module_pos,
                              v2_add(v2_scale(outward, radial),
                                     v2_scale(tangent, lateral))),
                .vel = v2(0.0f, 0.0f),
                .radius = 18.0f,
                .rotation = 0.22f * (float)(p + 1),
                .age = 2.0f,
            };
            for (uint16_t i = 0; i < pod->manifest_count; i++) {
                pod->manifest_units[i] = (cargo_unit_t){
                    .kind = CARGO_KIND_INGOT,
                    .commodity = COMMODITY_FERRITE_INGOT,
                    .grade = (uint8_t)MINING_GRADE_COMMON,
                    .quantity = 1,
                    .origin_station = 0,
                };
            }
            cargo_pod_set_module_tractor(pod, 0, module_idx);
            vec2 emitter = module_pos;
            if (!station_module_tractor_emitter(
                    &g.world, 0, module_idx, pod->pos, &emitter)) {
                return 0;
            }
            int interaction_idx = g.world.interactions.count++;
            g.world.interactions.items[interaction_idx] = (sim_interaction_t){
                .type = SIM_INTERACTION_TRACTOR_BEAM,
                .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
                .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
                .source = {
                    .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
                    .index = 0,
                    .aux = (int16_t)module_idx,
                },
                .target = {
                    .type = SIM_INTERACTION_ENTITY_CARGO_POD,
                    .index = (int16_t)pod_idx,
                    .aux = -1,
                },
                .source_pos = emitter,
                .target_pos = pod->pos,
                .range = CARGO_POD_DOCK_TRACTOR_RANGE,
                .intensity = 0.60f + 0.12f * p,
            };
        }

        vec2 camera = v2_add(module_pos, v2_scale(outward, 165.0f));
        sp->ship->pos = v2_add(camera, v2_scale(tangent, 185.0f));
        sp->ship->vel = v2(0.0f, 0.0f);
        g.camera_pos = camera;
        g.camera_initialized = true;
        return 1;
    }
    case SMOKE_LOOP_STATE_STATION_FRAGMENT_TRACTOR: {
        if (g.world.station_count <= 0 ||
            !station_exists(&g.world.stations[0])) {
            return 0;
        }
        /* Hold a real station/fragment interaction tableau at the render
         * boundary. This exercises the exact beam resolver and draw order
         * used by furnace/hopper tractors while keeping the scene stable
         * enough to measure render-buffer pressure. */
        g.local_server.active = false;
        station_t *st = &g.world.stations[0];
        int source_modules[2] = {-1, -1};
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            if (source_modules[0] < 0) {
                source_modules[0] = m;
            } else {
                source_modules[1] = m;
                break;
            }
        }
        if (source_modules[0] < 0 || source_modules[1] < 0) return 0;

        vec2 source_a = module_world_pos_ring(
            st, st->modules[source_modules[0]].ring,
            st->modules[source_modules[0]].slot);
        vec2 source_b = module_world_pos_ring(
            st, st->modules[source_modules[1]].ring,
            st->modules[source_modules[1]].slot);
        vec2 midpoint = v2_scale(v2_add(source_a, source_b), 0.5f);
        vec2 axis = v2_norm(v2_sub(source_b, source_a));
        vec2 normal = v2(-axis.y, axis.x);
        vec2 target = v2_add(midpoint, v2_scale(normal, 115.0f));
        smoke_seed_asteroid(0, ASTEROID_TIER_S,
                            COMMODITY_FERRITE_ORE, target,
                            14.0f, 1.0f, 607.0f);
        g.world.asteroids[0].fracture_child = true;

        g.world.interactions.count = 0;
        for (int beam = 0; beam < 2; beam++) {
            int module_idx = source_modules[beam];
            vec2 emitter = module_world_pos_ring(
                st, st->modules[module_idx].ring,
                st->modules[module_idx].slot);
            if (!station_module_tractor_emitter(
                    &g.world, 0, module_idx, target, &emitter)) {
                return 0;
            }
            g.world.interactions.items[g.world.interactions.count++] =
                (sim_interaction_t){
                    .type = SIM_INTERACTION_TRACTOR_BEAM,
                    .visual =
                        SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR,
                    .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
                    .source = {
                        .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
                        .index = 0,
                        .aux = (int16_t)module_idx,
                    },
                    .target = {
                        .type = SIM_INTERACTION_ENTITY_ASTEROID,
                        .index = 0,
                        .aux = -1,
                    },
                    .source_pos = emitter,
                    .target_pos = target,
                    .range = HOPPER_PULL_RANGE,
                    .intensity = 0.82f,
                };
        }

        sp->docked = false;
        sp->current_station = -1;
        sp->nearby_station = 0;
        sp->in_dock_range = true;
        sp->ship->pos = v2_add(midpoint, v2_scale(normal, 250.0f));
        sp->ship->vel = v2(0.0f, 0.0f);
        g.camera_pos = midpoint;
        g.camera_initialized = true;
        return 1;
    }
    case SMOKE_LOOP_STATE_HAIL_READY: {
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        g.local_server.active = true;
        g.world.stations[0].ledger_count = 0;
        bool has_token = false;
        for (int i = 0; i < 8; i++) {
            if (sp->session_token[i] != 0) {
                has_token = true;
                break;
            }
        }
        if (!has_token) {
            const uint8_t smoke_token[8] = {
                'S', 'M', 'O', 'K', 'E', '0', '0', '1'
            };
            memcpy(sp->session_token, smoke_token, sizeof(sp->session_token));
            sp->session_ready = true;
        }
        ledger_earn(&g.world.stations[0], sp->session_token, 123.0f);
        g.known_station_ledger_count = 1;
        g.known_station_ledger[0] = (NetKnownLedgerEntry){
            .station = 0,
            .balance = 123.0f,
        };
        return 1;
    }
    case SMOKE_LOOP_STATE_HAIL_NOTICE:
        snprintf(g.notice, sizeof(g.notice),
                 "Prospect: channel open. Balance 123 cr.");
        g.notice_timer = 6.0f;
        snprintf(g.hail_station, sizeof(g.hail_station), "Prospect");
        snprintf(g.hail_message, sizeof(g.hail_message), "channel open");
        g.hail_credits = 123.0f;
        g.hail_timer = 6.0f;
        g.hail_station_index = 0;
        return 1;
    case SMOKE_LOOP_STATE_PLAN_GHOST:
        g.plan_mode_active = true;
        g.plan_target_station = -1;
        g.placement_target_station = -1;
        g.placement_target_ring = 1;
        g.placement_target_slot = 2;
        g.plan_type = MODULE_SIGNAL_RELAY;
        return 1;
    case SMOKE_LOOP_STATE_PLAN_SLOT:
        g.plan_mode_active = true;
        g.plan_target_station = 0;
        g.placement_target_station = 0;
        g.placement_target_ring = 1;
        g.placement_target_slot = 0;
        g.plan_type = MODULE_HOPPER;
        return 1;
    case SMOKE_LOOP_STATE_SCAFFOLD_SNAP:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.scaffolds[0].active = true;
        g.world.scaffolds[0].state = SCAFFOLD_SNAPPING;
        g.world.scaffolds[0].module_type = MODULE_FURNACE;
        g.world.scaffolds[0].placed_station = SMOKE_OUTPOST_INDEX;
        g.world.scaffolds[0].placed_ring = 2;
        g.world.scaffolds[0].placed_slot = 3;
        g.world.scaffolds[0].pos = sp->ship->pos;
        return 1;
    case SMOKE_LOOP_STATE_SUPPLY_NEED:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].pos = sp->ship->pos;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold = true;
        g.world.stations[SMOKE_OUTPOST_INDEX].planned = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold_progress = 0.5f;
        g.world.stations[SMOKE_OUTPOST_INDEX].dock_radius = OUTPOST_DOCK_RADIUS;
        return 1;
    case SMOKE_LOOP_STATE_YARD_BLOCKED:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].pos = sp->ship->pos;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].planned = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold_progress = 1.0f;
        g.world.stations[SMOKE_OUTPOST_INDEX].signal_range = 6000.0f;
        g.world.stations[SMOKE_OUTPOST_INDEX].dock_radius = OUTPOST_DOCK_RADIUS;
        g.world.stations[SMOKE_OUTPOST_INDEX].module_count = 1;
        g.world.stations[SMOKE_OUTPOST_INDEX].modules[0] = (station_module_t){
            .type = MODULE_SHIPYARD,
            .ring = 2,
            .slot = 0,
            .scaffold = false,
            .build_progress = 1.0f,
        };
        g.world.stations[SMOKE_OUTPOST_INDEX].pending_scaffold_count = 1;
        g.world.stations[SMOKE_OUTPOST_INDEX].pending_scaffolds[0].type = MODULE_FURNACE;
        g.world.stations[SMOKE_OUTPOST_INDEX].pending_scaffolds[0].owner = 0;
        g.world.scaffolds[0].active = true;
        g.world.scaffolds[0].state = SCAFFOLD_LOOSE;
        g.world.scaffolds[0].module_type = MODULE_DOCK;
        g.world.scaffolds[0].pos = sp->ship->pos;
        g.world.scaffolds[0].built_at_station = -1;
        scaffold_clear_tractor(&g.world.scaffolds[0]);
        return 1;
    case SMOKE_LOOP_STATE_ABANDONED_PLAN:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].pos = sp->ship->pos;
        g.world.stations[SMOKE_OUTPOST_INDEX].planned = true;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].placement_plan_count = 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].dock_radius = 0.0f;
        return 1;
    case SMOKE_LOOP_STATE_FRACTURE_TABLEAU: {
        g.local_server.active = false;
        sp->docked = false;
        sp->current_station = -1;
        sp->nearby_station = -1;
        sp->in_dock_range = false;
        sp->docking_approach = false;
        g.was_docked = false;
        g.dock_settle_timer = 0.0f;
        vec2 base = sp->ship->pos;
        if (g.world.station_count > 0 && station_exists(&g.world.stations[0]))
            base = v2_add(g.world.stations[0].pos, v2(-1250.0f, -820.0f));
        sp->ship->pos = base;
        sp->ship->angle = 0.0f;
        g.camera_pos = base;
        for (int i = 0; i < 12 && i < MAX_ASTEROIDS; i++) {
            memset(&g.world.asteroids[i], 0, sizeof(g.world.asteroids[i]));
        }
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            g.world.npc_ships[i].active = false;
        }
        sp->hover_asteroid = 0;
        smoke_seed_asteroid(0, ASTEROID_TIER_L, COMMODITY_FERRITE_ORE,
                            v2_add(base, v2(-190.0f, -96.0f)),
                            44.0f, 0.11f, 12.3f);
        smoke_seed_asteroid(1, ASTEROID_TIER_M, COMMODITY_CUPRITE_ORE,
                            v2_add(base, v2(170.0f, -110.0f)),
                            27.0f, 0.30f, 41.0f);
        smoke_seed_asteroid(2, ASTEROID_TIER_M, COMMODITY_FERRITE_ORE,
                            v2_add(base, v2(76.0f, 126.0f)),
                            26.0f, 0.78f, 83.0f);
        g.world.asteroids[2].fracture_child = true;
        g.world.asteroids[2].vel = v2(46.0f, -8.0f);
        g.world.asteroids[2].age = 0.03f;
        smoke_seed_asteroid(3, ASTEROID_TIER_M, COMMODITY_FERRITE_ORE,
                            v2_add(base, v2(10.0f, 182.0f)),
                            23.0f, 0.82f, 91.0f);
        g.world.asteroids[3].fracture_child = true;
        g.world.asteroids[3].vel = v2(-28.0f, 38.0f);
        g.world.asteroids[3].age = 0.08f;
        smoke_seed_asteroid(4, ASTEROID_TIER_S, COMMODITY_FERRITE_ORE,
                            v2_add(base, v2(144.0f, 194.0f)),
                            14.0f, 1.0f, 66.0f);
        g.world.asteroids[4].fracture_child = true;
        g.world.asteroids[4].vel = v2(54.0f, 42.0f);
        g.world.asteroids[4].age = 0.12f;
        memcpy(g.asteroid_interp.curr, g.world.asteroids, sizeof(g.asteroid_interp.curr));
        memcpy(g.asteroid_interp.prev, g.world.asteroids, sizeof(g.asteroid_interp.prev));
        memset(g.asteroid_interp.elapsed, 0,
               sizeof(g.asteroid_interp.elapsed));
        memset(g.npc_interp.curr, 0, sizeof(g.npc_interp.curr));
        memset(g.npc_interp.prev, 0, sizeof(g.npc_interp.prev));
        g.npc_interp.t = 0.0f;
        g.npc_interp.interval = 0.1f;
        return 1;
    }
    case SMOKE_LOOP_STATE_CUPRITE_GATE:
        g.local_server.active = false;
        sp->docked = false;
        sp->current_station = -1;
        sp->nearby_station = -1;
        sp->ship->mining_level = 0;
        sp->hover_asteroid = 0;
        smoke_seed_asteroid(0, ASTEROID_TIER_M, COMMODITY_CUPRITE_ORE,
                            v2_add(sp->ship->pos, v2(90.0f, 0.0f)),
                            28.0f, 1.0f, 123.0f);
        return 1;
    case SMOKE_LOOP_STATE_TRACKED_CUPRITE_CONTRACT:
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        g.local_server.active = false;
        sp->docked = false;
        sp->current_station = -1;
        sp->nearby_station = -1;
        sp->ship->mining_level = 0;
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            g.world.asteroids[i].active = false;
        g.onboarding.moved = true;
        g.onboarding.fractured = true;
        g.onboarding.tractored = true;
        g.onboarding.threw = true;
        g.onboarding.hailed = true;
        g.onboarding.earned = true;
        g.onboarding.docked_after_earning = true;
        g.onboarding.viewed_trade = true;
        g.onboarding.complete = true;
        g.onboarding.welcomed = true;
        g.tracked_contract = 0;
        g.selected_contract = 0;
        g.player_known_contract_mask = 1u;
        memset(&g.world.contracts[0], 0, sizeof(g.world.contracts[0]));
        g.world.contracts[0] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = 0,
            .commodity = COMMODITY_CUPRITE_ORE,
            .required_grade = MINING_GRADE_COMMON,
            .quantity_needed = 10.0f,
            .base_price = 120.0f,
            .target_pos = g.world.stations[0].pos,
            .claimed_by = -1,
        };
        return 1;
    case SMOKE_LOOP_STATE_ONBOARDING_DELIVER:
        smoke_set_onboarding_economy_progress(false, false, false);
        sp->docked = false;
        sp->ship->towed_count = 1;
        sp->ship->towed_fragments[0] = 0;
        return 1;
    case SMOKE_LOOP_STATE_ONBOARDING_RETURN:
        smoke_set_onboarding_economy_progress(true, false, false);
        sp->docked = false;
        return 1;
    case SMOKE_LOOP_STATE_ONBOARDING_MARKET:
        smoke_set_onboarding_economy_progress(true, true, false);
        sp->docked = true;
        g.station_view = STATION_VIEW_DOCK;
        return 1;
    case SMOKE_LOOP_STATE_ONBOARDING_COMPLETE:
        smoke_set_onboarding_economy_progress(true, true, true);
        sp->docked = true;
        g.station_view = STATION_VIEW_TRADE;
        g.onboarding.welcomed = false;
        return 1;
    case SMOKE_LOOP_STATE_SCAN_LASER_FAB: {
        if (g.world.station_count <= 2 || !station_exists(&g.world.stations[2]))
            return 0;
        int module_idx = -1;
        for (int i = 0; i < g.world.stations[2].module_count; i++) {
            if (g.world.stations[2].modules[i].type == MODULE_LASER_FAB) {
                module_idx = i;
                break;
            }
        }
        if (module_idx < 0) return 0;
        g.local_server.active = false;
        sp->docked = false;
        sp->scan_active = true;
        sp->scan_target_type = 1;
        sp->scan_target_index = 2;
        sp->scan_module_index = module_idx;
        return 1;
    }
    case SMOKE_LOOP_STATE_REMOTE_PILOT_SCAN: {
        int remote_slot = (g.local_player_slot == 1) ? 2 : 1;
        if (remote_slot >= NET_MAX_PLAYERS) return 0;
        g.net_authority_enabled = true;
        g.local_server.active = false;
        memset(&g.player_interp, 0, sizeof(g.player_interp));
        g.player_interp.interval = 0.1f;

        sp->ship->pos = v2_add(sp->ship->pos, v2(-120.0f, 0.0f));
        sp->ship->angle = 0.0f;
        sp->ship->tractor_level = 0;
        g.camera_pos = sp->ship->pos;

        NetPlayerState remote = {0};
        remote.active = true;
        remote.player_id = (uint8_t)remote_slot;
        remote.x = sp->ship->pos.x + ship_tractor_range(sp->ship) * 0.72f;
        remote.y = sp->ship->pos.y + 24.0f;
        remote.angle = 0.35f;
        remote.flags = 16; /* tractor active */
        remote.tractor_level = 1;
        remote.towed_count = 2;
        snprintf(remote.callsign, sizeof(remote.callsign), "VX4-201");
        g.player_interp.curr[remote_slot] = remote;
        g.player_interp.prev[remote_slot] = remote;
        return 1;
    }
    case SMOKE_LOOP_STATE_WEAK_SIGNAL_VISUAL:
        /* Keep the deterministic perception fixture focused on the live HUD
         * cue. The narrative episode is asynchronous in the browser and can
         * otherwise cover later review captures from the same page. */
        g.hail_ping_timer = 0.0f;
        return smoke_maintain_weak_signal_view() ? 1 : 0;
    case SMOKE_LOOP_STATE_NARROW_CAMERA_OFFSET: {
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        g.local_server.active = false;
        sp->docked = false;
        sp->current_station = -1;
        sp->nearby_station = -1;
        sp->in_dock_range = false;
        sp->docking_approach = false;
        g.was_docked = false;
        g.dock_settle_timer = 0.0f;
        vec2 base = v2_add(g.world.stations[0].pos, v2(90.0f, -80.0f));
        sp->ship->pos = base;
        sp->ship->vel = v2(0.0f, 0.0f);
        sp->ship->angle = 0.0f;
        g.camera_pos = v2_add(base, v2(-210.0f, 170.0f));
        g.camera_initialized = true;
        g.camera_drift_timer = 0.0f;
        g.camera_station_index = -1;
        g.boost_zoom = 1.0f;
        g.boost_center_blend = 0.0f;
        g.hail_ping_timer = 0.0f;
        g.hail_ping_origin = sp->ship->pos;
        g.signal_visual_saturation = signal_visual_saturation(
            signal_strength_at(&g.world, sp->ship->pos));
        g.signal_visual_saturation_initialized = true;
        return 1;
    }
    case SMOKE_LOOP_STATE_ROCK_SMELT_PATH:
        if (g.world.station_count <= 0 ||
            !station_is_active(&g.world.stations[0])) {
            return 0;
        }
        g.local_server.active = false;
        sp->hover_asteroid = 0;
        smoke_seed_asteroid(0, ASTEROID_TIER_S, COMMODITY_FERRITE_ORE,
                            v2_add(sp->ship->pos, v2(90.0f, 0.0f)),
                            12.0f, 1.0f, 211.0f);
        g.world.asteroids[0].grade = (uint8_t)MINING_GRADE_COMMON;
        for (int i = 0; i < g.world.station_count && i < MAX_STATIONS; i++)
            g.world.stations[i]._inventory_cache[COMMODITY_FERRITE_ORE] =
                REFINERY_HOPPER_CAPACITY;
        return 1;
    case SMOKE_LOOP_STATE_ROCK_ROUTE_TARGET:
        g.local_server.active = false;
        return smoke_seed_rock_route(false, false) ? 1 : 0;
    case SMOKE_LOOP_STATE_ROCK_ROUTE_TOW:
        g.local_server.active = false;
        return smoke_seed_rock_route(true, false) ? 1 : 0;
    case SMOKE_LOOP_STATE_ROCK_ROUTE_DEGRADED:
        g.local_server.active = false;
        return smoke_seed_rock_route(true, true) ? 1 : 0;
    case SMOKE_LOOP_STATE_CARGO_LINEAGE: {
        if (g.world.station_count <= 0 ||
            !station_exists(&g.world.stations[0])) return 0;
        station_t *st = &g.world.stations[0];
        int dock_module = -1;
        for (int m = 0; m < st->module_count; m++) {
            if (!st->modules[m].scaffold &&
                st->modules[m].type == MODULE_DOCK) {
                dock_module = m;
                break;
            }
        }
        if (dock_module < 0) return 0;

        uint8_t fragment_pub[32];
        for (int i = 0; i < 32; i++)
            fragment_pub[i] = (uint8_t)(0x31 + i);
        cargo_unit_t ingot = {0};
        cargo_unit_t frame = {0};
        if (!hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                        fragment_pub, 0, &ingot)) return 0;
        ingot.origin_station = 0;
        ingot.mined_block = 4422;
        if (!hash_product(RECIPE_FRAME_BASIC, &ingot, 1, 0, &frame)) return 0;
        frame.origin_station = 0;

        if (g.local_server.world.station_count > 0) {
            station_t *authority = &g.local_server.world.stations[0];
            chain_payload_smelt_t smelt = {0};
            memcpy(smelt.fragment_pub, fragment_pub, 32);
            memcpy(smelt.ingot_pub, ingot.pub, 32);
            smelt.prefix_class = ingot.prefix_class;
            smelt.mined_block = ingot.mined_block;
            (void)chain_log_emit(&g.local_server.world, authority,
                                 CHAIN_EVT_SMELT, &smelt, sizeof(smelt));

            chain_payload_craft_t craft = {0};
            craft.recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
            craft.input_count = 1;
            memcpy(craft.output_pub, frame.pub, 32);
            memcpy(craft.input_pubs[0], ingot.pub, 32);
            (void)chain_log_emit(&g.local_server.world, authority,
                                 CHAIN_EVT_CRAFT, &craft, sizeof(craft));
        }

        reset_trade_session_rows(0);
        return smoke_maintain_cargo_lineage_view() ? 1 : 0;
    }
    case SMOKE_LOOP_STATE_LOCAL_MONEY:
        return smoke_maintain_local_money_view() ? 1 : 0;
    case SMOKE_LOOP_STATE_NPC_MOTIVE_CRISP:
        return smoke_maintain_npc_motive_view(false) ? 1 : 0;
    case SMOKE_LOOP_STATE_NPC_MOTIVE_DEGRADED:
        return smoke_maintain_npc_motive_view(true) ? 1 : 0;
    case SMOKE_LOOP_STATE_REMEMBERED_WORK_CRISP:
        return smoke_seed_remembered_work(false) ? 1 : 0;
    case SMOKE_LOOP_STATE_REMEMBERED_WORK_DEGRADED:
        return smoke_seed_remembered_work(true) ? 1 : 0;
    case SMOKE_LOOP_STATE_CONSTRUCTION_CONSEQUENCE:
        return smoke_maintain_construction_consequence_view() ? 1 : 0;
    default:
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE
int set_smoke_loop_state(int state) {
    if (!smoke_hooks_enabled()) {
        smoke_loop_state_override = 0;
        return 0;
    }
    int ok = smoke_apply_loop_state(state);
    smoke_loop_state_override = ok ? state : 0;
    return ok;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_construction_state_mask(void) {
    if (g.world.station_count <= 0 ||
        !station_exists(&g.world.stations[0])) return 0;
    int mask = 0;
    const station_t *st = &g.world.stations[0];
    for (int i = 0; i < st->module_count; i++) {
        switch (module_build_state(&st->modules[i])) {
        case MODULE_BUILD_AWAITING_SUPPLY: mask |= 1 << 0; break;
        case MODULE_BUILD_BUILDING:        mask |= 1 << 1; break;
        case MODULE_BUILD_COMPLETE:        mask |= 1 << 2; break;
        }
    }
    return mask;
}
#endif

/* ------------------------------------------------------------------ */
/* draw_hud_panels -- background panel geometry for the flight HUD     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Hull-fog textures — generated radial vignettes, one per damage tier */
/* ------------------------------------------------------------------ */

#define HULL_FOG_LEVELS 4
#define HULL_FOG_TEX_SIZE 256

static struct {
    bool initialized;
    uint32_t image_id[HULL_FOG_LEVELS];
    uint32_t view_id[HULL_FOG_LEVELS];
    uint32_t sampler_id;
    uint32_t blend_pip_id;    /* sgl pipeline with alpha blending enabled */
} hull_fog;

static float fog_smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

void hull_fog_init(void) {
    if (hull_fog.initialized) return;

    sg_sampler samp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    hull_fog.sampler_id = samp.id;

    /* Custom sokol_gl pipeline with alpha blending enabled. The default
     * sgl pipeline has blend.enabled=false and write_mask=RGB, so any
     * alpha in vertex colors or textures is completely ignored. */
    sgl_pipeline blend_pip = sgl_make_pipeline(&(sg_pipeline_desc){
        .colors[0] = {
            .write_mask = SG_COLORMASK_RGBA,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
    });
    hull_fog.blend_pip_id = blend_pip.id;

    /* Generate one radial vignette per damage tier. The "clear hole" in
     * the middle gets smaller and the surrounding fog gets darker as
     * the tier rises. RGB is white so the vertex color can tint it. */
    static const float clear_radius[HULL_FOG_LEVELS] = {
        0.70f, /* tier 0: caution — barely any darkening */
        0.50f, /* tier 1: warn */
        0.32f, /* tier 2: danger */
        0.18f, /* tier 3: critical — tight aperture */
    };
    static const float peak_alpha[HULL_FOG_LEVELS] = {
        0.45f, 0.65f, 0.82f, 0.95f,
    };

    /* Separate buffer per tier so the immutable image uploads each
     * see distinct data, no aliasing.
     *
     * The fog edge isn't a clean circle — it's deformed by a stack of
     * angular cosines so fingers reach inward at irregular intervals.
     * Multiple frequencies + per-octave phase offsets keep them from
     * lining up into a clean star. The deformation is enveloped so it
     * vanishes at the center (clear hole stays clean) and at the
     * corners (full coverage holds). Result reads as "rivers of blood
     * pulling in from the edges" rather than a circular vignette,
     * while the smoothstep falloff keeps everything blurry. */
    static uint8_t pixels[HULL_FOG_LEVELS][HULL_FOG_TEX_SIZE * HULL_FOG_TEX_SIZE * 4];
    for (int level = 0; level < HULL_FOG_LEVELS; level++) {
        uint8_t *pix = pixels[level];
        float r0 = clear_radius[level];
        float r1 = 1.10f; /* fog reaches full alpha just past the corners */
        float pa = peak_alpha[level];
        /* Tendril depth scales with tier — barely visible at caution,
         * pronounced fingers at critical. */
        float tendril_amp = 0.06f + 0.10f * ((float)level / (float)(HULL_FOG_LEVELS - 1));
        for (int y = 0; y < HULL_FOG_TEX_SIZE; y++) {
            for (int x = 0; x < HULL_FOG_TEX_SIZE; x++) {
                float fx = ((float)x / (float)(HULL_FOG_TEX_SIZE - 1)) * 2.0f - 1.0f;
                float fy = ((float)y / (float)(HULL_FOG_TEX_SIZE - 1)) * 2.0f - 1.0f;
                float d_radial = sqrtf(fx * fx + fy * fy);
                float theta = atan2f(fy, fx);
                /* Three octaves of cosine + irrational-ish phase offsets
                 * so the period is long and the pattern looks organic. */
                float tendril =
                    0.55f * cosf(theta *  5.0f + 0.0f)
                  + 0.32f * cosf(theta *  9.0f + 1.7f)
                  + 0.18f * cosf(theta * 17.0f + 0.4f);
                /* Envelope: 4 * d * (1-d) peaks at d=0.5, 0 at d=0 and d=1.
                 * Keeps the clear center round and the corners saturated. */
                float env = 4.0f * d_radial * (1.0f - d_radial);
                if (env < 0.0f) env = 0.0f;
                float d = d_radial - tendril_amp * tendril * env;
                float a = fog_smoothstep(r0, r1, d) * pa;
                int p = (y * HULL_FOG_TEX_SIZE + x) * 4;
                pix[p + 0] = 255;
                pix[p + 1] = 255;
                pix[p + 2] = 255;
                pix[p + 3] = (uint8_t)(a * 255.0f);
            }
        }
        /* IMMUTABLE image with .data inline. sg_make_image uploads
         * synchronously and bypasses sg_update_image's frame-index
         * check that silently drops updates at init time
         * (upd_frame_index == frame_index == 0 → validation fails). */
        sg_image img = sg_make_image(&(sg_image_desc){
            .width = HULL_FOG_TEX_SIZE,
            .height = HULL_FOG_TEX_SIZE,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .data.mip_levels[0] = {
                .ptr = pix,
                .size = (size_t)(HULL_FOG_TEX_SIZE * HULL_FOG_TEX_SIZE * 4),
            },
        });
        sg_view view = sg_make_view(&(sg_view_desc){ .texture.image = img });
        hull_fog.image_id[level] = img.id;
        hull_fog.view_id[level] = view.id;
    }
    hull_fog.initialized = true;
}

void hull_fog_shutdown(void) {
    if (hud_alpha_pipeline_id) {
        sgl_destroy_pipeline((sgl_pipeline){ hud_alpha_pipeline_id });
        hud_alpha_pipeline_id = 0;
    }

    if (!hull_fog.initialized) return;

    if (hull_fog.blend_pip_id) {
        sgl_destroy_pipeline((sgl_pipeline){ hull_fog.blend_pip_id });
        hull_fog.blend_pip_id = 0;
    }
    for (int level = 0; level < HULL_FOG_LEVELS; level++) {
        if (hull_fog.view_id[level]) {
            sg_destroy_view((sg_view){ hull_fog.view_id[level] });
            hull_fog.view_id[level] = 0;
        }
        if (hull_fog.image_id[level]) {
            sg_destroy_image((sg_image){ hull_fog.image_id[level] });
            hull_fog.image_id[level] = 0;
        }
    }
    if (hull_fog.sampler_id) {
        sg_destroy_sampler((sg_sampler){ hull_fog.sampler_id });
        hull_fog.sampler_id = 0;
    }
    hull_fog.initialized = false;
}

/* Shared lava-lamp pulse — slow multi-frequency drift, no spikes. */
static float fog_pulse(void) {
    float t = g.world.time;
    float wob = 0.55f * sinf(t * 0.43f)
              + 0.30f * sinf(t * 0.71f + 1.3f)
              + 0.15f * sinf(t * 1.07f + 2.7f);
    return 0.92f + 0.08f * wob;
}

/* Emit the textured fog quads for one wave at the given intensity. Picks
 * the tier-pair around `intensity` and crossfades them so the aperture
 * tightens smoothly. Caller is responsible for setting up screen-space
 * ortho before calling. */
static void draw_fog_quads(float intensity, float pulse) {
    if (!hull_fog.initialized) return;
    if (intensity <= 0.01f) return;
    if (intensity > 1.0f) intensity = 1.0f;

    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();

    /* Continuous tier value in [0, HULL_FOG_LEVELS - 1]. Lerp the texture
     * pair around it so transitions are smooth across the whole range. */
    float tier = intensity * (float)(HULL_FOG_LEVELS - 1);
    int t0 = (int)floorf(tier);
    if (t0 < 0) t0 = 0;
    if (t0 > HULL_FOG_LEVELS - 1) t0 = HULL_FOG_LEVELS - 1;
    int t1 = t0 + 1;
    if (t1 > HULL_FOG_LEVELS - 1) t1 = HULL_FOG_LEVELS - 1;
    float blend = tier - (float)t0;
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;

    /* Dark blood red tint. Brightens slightly with intensity.
     * Locals prefixed with tint_ to avoid shadowing the global `g` game_t. */
    float tint_r = 0.20f + 0.25f * intensity;
    float tint_g = 0.005f + 0.01f * intensity;
    float tint_b = 0.01f + 0.02f * intensity;

    /* Push our alpha-blending pipeline so the texture's alpha actually
     * affects the framebuffer. The default sokol_gl pipeline disables
     * blending and writes RGB only — alpha is silently discarded. */
    sgl_push_pipeline();
    sgl_load_pipeline((sgl_pipeline){ hull_fog.blend_pip_id });
    sgl_enable_texture();

    /* Lower tier (full strength) */
    {
        float a = (1.0f - blend) * pulse;
        sgl_texture(
            (sg_view){ hull_fog.view_id[t0] },
            (sg_sampler){ hull_fog.sampler_id });
        sgl_begin_quads();
        sgl_c4f(tint_r, tint_g, tint_b, a);
        sgl_v2f_t2f(0.0f,     0.0f,     0.0f, 0.0f);
        sgl_v2f_t2f(screen_w, 0.0f,     1.0f, 0.0f);
        sgl_v2f_t2f(screen_w, screen_h, 1.0f, 1.0f);
        sgl_v2f_t2f(0.0f,     screen_h, 0.0f, 1.0f);
        sgl_end();
    }

    /* Upper tier (blended weight) */
    if (t1 != t0) {
        float a = blend * pulse;
        sgl_texture(
            (sg_view){ hull_fog.view_id[t1] },
            (sg_sampler){ hull_fog.sampler_id });
        sgl_begin_quads();
        sgl_c4f(tint_r, tint_g, tint_b, a);
        sgl_v2f_t2f(0.0f,     0.0f,     0.0f, 0.0f);
        sgl_v2f_t2f(screen_w, 0.0f,     1.0f, 0.0f);
        sgl_v2f_t2f(screen_w, screen_h, 1.0f, 1.0f);
        sgl_v2f_t2f(0.0f,     screen_h, 0.0f, 1.0f);
        sgl_end();
    }

    sgl_disable_texture();
    sgl_pop_pipeline();
}

/* Back wave — drawn before the world. Ramps 0..1 over damage 0..0.5
 * (HP 100→50), then holds at 1. Caller (render_frame) sets up screen
 * ortho. Renders the dark void deepening behind ships/stations without
 * eating their contrast. */
void draw_hull_fog_back(void) {
    if (!hull_fog.initialized) return;
    float damage = g.fog_intensity;
    if (damage <= 0.01f) return;
    float intensity = damage * 2.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    float pulse = fog_pulse() + 0.04f * intensity;
    draw_fog_quads(intensity, pulse);
}

/* Front wave — drawn over the world (HUD pass). Closing-in aperture that
 * only kicks in below 50% HP, ramping 0..1 over damage 0.5..1.0. */
static void draw_hull_warning_overlay(void) {
    if (!hull_fog.initialized) return;
    float damage = g.fog_intensity;
    float intensity = (damage - 0.5f) * 2.0f;
    if (intensity <= 0.01f) return;
    if (intensity > 1.0f) intensity = 1.0f;
    float pulse = fog_pulse() + 0.04f * intensity;
    draw_fog_quads(intensity, pulse);
}

void draw_hud_panels(void) {
    if (g.death_screen_timer > 0.0f) return;
    /* Suppress HUD chrome during the death cinematic — we want a clean
     * shot of the wreckage and stats menu. */
    if (g.death_cinematic.active || g.death_cinematic.menu_alpha > 0.001f) {
        draw_hull_warning_overlay();
        return;
    }
    draw_hull_warning_overlay();
    float top_x = 0.0f, top_y = 0.0f, top_w = 0.0f, top_h = 0.0f;
    float bottom_x = 0.0f, bottom_y = 0.0f, bottom_w = 0.0f, bottom_h = 0.0f;
    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);

    /* Top flight HUD panel — suppressed while docked so the station terminal
     * is the only status surface in view. */
    if (!(LOCAL_PLAYER.docked && g.dock_settle_timer <= 0.0f)) {
        draw_ui_panel(top_x, top_y, top_w, top_h, 0.03f);
    }

    if (LOCAL_PLAYER.docked && g.dock_settle_timer <= 0.0f) {
        float panel_x = 0.0f;
        float panel_y = 0.0f;
        float panel_w = 0.0f;
        float panel_h = 0.0f;
        bool compact = ui_is_compact();
        station_ui_state_t ui = { 0 };
        build_station_ui_state(&ui);
        get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
        draw_ui_scrim(0.34f);
        /* Tint panel accent by station role color */
        {
            float sr, sg, sb;
            station_role_color(ui.station, &sr, &sg, &sb);
            draw_ui_panel_colored(panel_x, panel_y, panel_w, panel_h, sr, sg, sb);
        }

        float inner_x = panel_x + 18.0f;

        /* Divider rule below the persistent header band (drawn by
         * station_ui.c at panel_y + 26/42/58). The rule visually separates
         * the always-visible header (name/role/balance/hull/hold/ticker)
         * from the per-view content body below. */
        draw_ui_rule(inner_x, panel_x + panel_w - 18.0f, panel_y + 72.0f,
            0.14f, 0.26f, 0.38f, 0.70f);

        /* Content body draws directly on the outer panel — no nested frame. */

        /* Ship status strip -- persistent labeled ship state.
         * Layout: "HULL" label, hull meter, "HULL N/N", gap, "CARGO" label,
         * cargo meter, "CARGO N/N", then "LSR N HLD N TRC N" on the right.
         * Pip rows replaced by inline text chips to match the terminal voice. */
        {
            float strip_y = panel_y + panel_h - (compact ? 32.0f : 38.0f);
            float label_y = strip_y + 6.0f;     /* text baseline inside strip */
            float meter_y = strip_y + 10.0f;    /* meter top */
            float meter_h = 8.0f;
            float meter_w = compact ? 80.0f : 100.0f;
            const float cell_w = 8.0f;

            const ship_t *ship = LOCAL_PLAYER.ship;
            int hull_n = (int)lroundf(ship->hull);
            int hull_m = (int)lroundf(ship_max_hull(ship));
            int carg_n = (int)lroundf(ship_total_cargo(ship));
            int carg_m = (int)lroundf(ship_cargo_capacity(ship));

            float x = inner_x + 6.0f;

            /* HULL label */
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts("HULL");
            x += 5.0f * cell_w;
            /* Hull meter */
            draw_ui_meter(x, meter_y, meter_w, meter_h,
                ship->hull / ship_max_hull(ship), 0.96f, 0.54f, 0.28f);
            x += meter_w + 6.0f;
            /* Hull numeric */
            char hull_buf[20];
            snprintf(hull_buf, sizeof(hull_buf), "%d/%d", hull_n, hull_m);
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts(hull_buf);
            x += ((float)strlen(hull_buf) + 2.0f) * cell_w;

            /* CARGO label */
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts("CARGO");
            x += 6.0f * cell_w;
            draw_ui_meter(x, meter_y, meter_w, meter_h,
                (float)carg_n / fmaxf(1.0f, (float)carg_m), 0.26f, 0.90f, 0.72f);
            x += meter_w + 6.0f;
            char carg_buf[20];
            snprintf(carg_buf, sizeof(carg_buf), "%d/%d", carg_n, carg_m);
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts(carg_buf);

            /* Upgrade chips — right-aligned inside the panel. */
            if (!compact) {
                char chips[32];
                snprintf(chips, sizeof(chips), "LSR %d  HLD %d  TRC %d",
                         ship->mining_level, ship->hold_level, ship->tractor_level);
                float chips_w = (float)strlen(chips) * cell_w;
                sdtx_color3b(PAL_NAV_BLUE);
                sdtx_pos(ui_text_pos(panel_x + panel_w - 22.0f - chips_w),
                         ui_text_pos(label_y));
                sdtx_puts(chips);
            }
        }
    }

    /* Message panel background removed — text only now */
}

/* ------------------------------------------------------------------ */
/* draw_hud -- the main HUD text layer                                 */
/* ------------------------------------------------------------------ */

/* Death overlay. Phase 0 keeps the world visible: wreckage tumbles,
 * critical warning flashes, and the screen fades slowly toward black.
 * Phase 1 fades in stats + "[E] launch". Returns true when it owns the
 * HUD layer so regular flight chrome stays hidden. */
static bool draw_death_overlay(float screen_w, float screen_h) {
    bool active = g.death_cinematic.active;
    if (!active && g.death_cinematic.menu_alpha <= 0.001f)
        return false;

    float menu_alpha = g.death_cinematic.menu_alpha;
    if (menu_alpha < 0.0f) menu_alpha = 0.0f;
    if (menu_alpha > 1.0f) menu_alpha = 1.0f;
    float age = active ? g.death_cinematic.age : DEATH_CINEMATIC_FADE_TO_BLACK_SEC;
    float fade_t = active ? clampf(age / DEATH_CINEMATIC_FADE_TO_BLACK_SEC, 0.0f, 1.0f)
                          : menu_alpha;
    fade_t = fade_t * fade_t * (3.0f - 2.0f * fade_t);
    float scrim = fmaxf(0.74f * fade_t, 0.55f * menu_alpha);
    if (scrim > 0.001f)
        hud_draw_alpha_rect(0.0f, 0.0f, screen_w, screen_h, 0.0f, 0.0f, 0.0f, scrim);

    /* 1:1 canvas so text fills the screen. */
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    float cx = screen_w * 0.5f;
    float cy = screen_h * 0.5f;
    float cell = (screen_w < 380.0f) ? 7.0f : 8.0f;

    if (active && age < DEATH_CINEMATIC_WARNING_SEC) {
        float warning_fade = clampf((DEATH_CINEMATIC_WARNING_SEC - age) / 0.8f, 0.0f, 1.0f);
        float blink = (sinf(g.world.time * 18.0f) > 0.0f) ? 1.0f : 0.20f;
        uint8_t wa = (uint8_t)(255.0f * warning_fade * blink);
        sdtx_color4b(PAL_DEATH_PROMPT, wa);
        sdtx_centered_text(cx, fmaxf(2.0f, (screen_h * 0.18f) / cell),
                           cell, "[ SYSTEM CRITICAL :::: SIGNAL LOST ]");
    }

    if (menu_alpha <= 0.001f) return true;

    float alpha = menu_alpha;
    uint8_t a8 = (uint8_t)(alpha * 255.0f);

    /* Title. */
    sdtx_color4b(PAL_DEATH_TITLE, a8);
    sdtx_centered_text(cx, (cy - 60.0f) / cell, cell, "SHIP DESTROYED");

    /* Stats. */
    float row = (cy - 16.0f) / cell;
    float left = fmaxf(1.0f, (cx - 110.0f) / cell);
    sdtx_color4b(PAL_NOTICE, a8);

    sdtx_pos(left, row);
    sdtx_printf("Ore smelted:   %8.0f", g.death_ore_mined);
    row += 2.5f;
    sdtx_pos(left, row);
    sdtx_printf("Rocks broken:  %8d", g.death_asteroids_fractured);
    row += 2.5f;
    sdtx_pos(left, row);
    sdtx_color4b(PAL_DEATH_EARNED, a8);
    sdtx_printf("Credits earned:%8.0f", g.death_credits_earned);
    row += 2.5f;
    sdtx_pos(left, row);
    sdtx_color4b(PAL_DEATH_SPENT, a8);
    sdtx_printf("Credits spent: %8.0f", g.death_credits_spent);
    row += 3.0f;

    /* Global highscores — top runs broadcast by the server. The
     * player's just-submitted run is highlighted in the death-earned
     * color so they can spot where they landed. Always render the
     * header so the player knows the leaderboard exists even when
     * empty (first run on a fresh server) or before the server's
     * post-death broadcast has arrived. */
    {
        sdtx_color4b(PAL_NOTICE, a8);
        sdtx_centered_text(cx, row, cell, "-- TOP RUNS --");
        row += 1.6f;
            if (g.highscore_count > 0) {
                int show = (g.highscore_count > 8) ? 8 : g.highscore_count;
                for (int i = 0; i < show; i++) {
                    char cs[9];
                    memcpy(cs, g.highscores[i].callsign, 8);
                    cs[8] = '\0';
                    for (int k = 7; k >= 0 && (cs[k] == ' ' || cs[k] == '\0'); k--) cs[k] = '\0';
                    char kb[9];
                    memcpy(kb, g.highscores[i].killed_by, 8);
                    kb[8] = '\0';
                    for (int k = 7; k >= 0 && (kb[k] == ' ' || kb[k] == '\0'); k--) kb[k] = '\0';
                    bool is_me = (g.death_credits_earned > 0.5f
                                  && fabsf(g.highscores[i].credits_earned - g.death_credits_earned) < 0.5f);
                    if (is_me) sdtx_color4b(PAL_DEATH_EARNED, a8);
                    else       sdtx_color4b(PAL_TEXT_FADED,  a8);
                    sdtx_pos(left, row);
                    /* callsign / credits / killed-by / build. world_id is
                     * kept in the data but dropped from the visible row
                     * to keep the line readable on narrow viewports. */
                    sdtx_printf("%2d. %-8s %7.0f  %-8s b#%08x",
                                i + 1, cs, g.highscores[i].credits_earned,
                                kb[0] ? kb : "-",
                                g.highscores[i].build_id);
                    row += 1.4f;
                }
            } else {
                sdtx_color4b(PAL_TEXT_FADED, a8);
                sdtx_pos(left, row);
                sdtx_puts("  (no records yet)");
                row += 1.4f;
            }
            row += 1.0f;
        }

    /* Prompt — RED, hard FLASH on/off. Includes the spawn fee that was
     * just debited at the respawn station so the player sees the cost
     * of dying ("respawn -300 Helios credits"). */
    float flash = (sinf(g.world.time * 7.0f) > 0.0f) ? 1.0f : 0.25f;
    uint8_t pa = (uint8_t)(flash * (float)a8);
    sdtx_color4b(PAL_DEATH_PROMPT, pa);
    char prompt[80];
    if (g.death_respawn_fee > 0.5f &&
        g.death_respawn_station < MAX_STATIONS &&
        g.world.stations[g.death_respawn_station].name[0]) {
        const char *cur =
            g.world.stations[g.death_respawn_station].currency_name[0]
            ? g.world.stations[g.death_respawn_station].currency_name
            : "credits";
        snprintf(prompt, sizeof(prompt),
                 "[ E ] launch  -%.0f %s",
                 g.death_respawn_fee, cur);
    } else {
        snprintf(prompt, sizeof(prompt), "[ E ] launch");
    }
    sdtx_centered_text(cx, row, cell, prompt);

    return true;
}

void draw_hud(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();

    /* Death-screen overlay short-circuits the rest of the HUD. */
    if (draw_death_overlay(screen_w, screen_h)) return;

    bool compact = ui_is_compact();
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float message_x = 0.0f;
    float message_y = 0.0f;
    float message_w = 0.0f;
    float message_h = 0.0f;
    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);
    float top_text_x = ui_text_pos(top_x + 16.0f);
    float top_row_0 = ui_text_pos(top_y + 16.0f);
    float top_row_1 = ui_text_pos(top_y + (compact ? 24.0f : 30.0f));
    float top_row_2 = ui_text_pos(top_y + (compact ? 32.0f : 44.0f));
    float top_row_3 = ui_text_pos(top_y + (compact ? 40.0f : 58.0f));
    char message_label[32] = { 0 };
    char message_text[320] = { 0 };
    /* Up to 4 wrapped lines for the subtitle. Long station hails land
     * here; empty slots are skipped on render. */
    enum { HUD_MSG_LINES = 4, HUD_MSG_LINE_CAP = 96 };
    char message_lines[HUD_MSG_LINES][HUD_MSG_LINE_CAP] = {{0}};
    uint8_t message_r = 164;
    uint8_t message_g = 177;
    uint8_t message_b = 205;
    int hull_units = (int)lroundf(LOCAL_PLAYER.ship->hull);
    int hull_capacity = (int)lroundf(ship_max_hull(LOCAL_PLAYER.ship));

    /* --- Low HP warning: pulsing red text in message area instead of vignette --- */
    /* (hull warning state is used by build_hud_message to show HULL INTEGRITY FAILING) */
    int cargo_units = (int)lroundf(ship_total_cargo(LOCAL_PLAYER.ship));
    int credits = (int)lroundf(player_current_balance());
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(LOCAL_PLAYER.ship));
    const station_t* current_station = current_station_ptr();
    const station_t* navigation_station = navigation_station_ptr();
    station_ui_state_t ui = { 0 };
    if (LOCAL_PLAYER.docked) {
        build_station_ui_state(&ui);
    }
    int station_distance = 0;

    vec2 forward = v2_from_angle(LOCAL_PLAYER.ship->angle);
    vec2 home = v2(0.0f, -1.0f);
    if (navigation_station != NULL) {
        station_distance = (int)lroundf(v2_len(v2_sub(navigation_station->pos, LOCAL_PLAYER.ship->pos)));
        home = v2_norm(v2_sub(navigation_station->pos, LOCAL_PLAYER.ship->pos));
    }
    float bearing = atan2f(v2_cross(forward, home), v2_dot(forward, home));
    int bearing_degrees = (int)lroundf(fabsf(bearing) * (180.0f / PI_F));
    const char* bearing_side = "ahead";
    if (bearing > 0.12f) {
        bearing_side = "left";
    } else if (bearing < -0.12f) {
        bearing_side = "right";
    } else {
        bearing_degrees = 0;
    }

    sdtx_canvas(screen_w / ui_text_zoom(), screen_h / ui_text_zoom());
    sdtx_font(0);
    sdtx_origin(0.0f, 0.0f);
    sdtx_home();
    if (hud_should_draw_message_panel()) {
        int message_cols = 0;
        get_hud_message_panel_rect(&message_x, &message_y, &message_w, &message_h);
        build_hud_message(message_label, sizeof(message_label), message_text, sizeof(message_text), &message_r, &message_g, &message_b);
        message_cols = (int)((message_w - 28.0f) / (HUD_CELL * ui_text_zoom()));
        /* Wrap into up to HUD_MSG_LINES word-wrapped lines. Long station
         * hails ("Station: MOTD  (balance N cur)") fit fully now; the
         * old 2-line splitter dropped everything past line 1. */
        wrap_hud_message_lines(message_text, message_cols, message_lines,
                               HUD_MSG_LINE_CAP, HUD_MSG_LINES);
    }

    float sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
    int sig_pct = (int)lroundf(sig_quality * 100.0f);
    const char* sig_band = signal_band_name(sig_quality);
    uint8_t sig_r, sig_g, sig_b;
    if (sig_quality < SIGNAL_BAND_FRONTIER) {
        const uint8_t c[3] = { PAL_SIGNAL_FRONTIER };
        sig_r = c[0]; sig_g = c[1]; sig_b = c[2];
    } else if (sig_quality < SIGNAL_BAND_FRINGE) {
        const uint8_t c[3] = { PAL_SIGNAL_FRINGE };
        sig_r = c[0]; sig_g = c[1]; sig_b = c[2];
    } else {
        const uint8_t c[3] = { PAL_SIGNAL_OPERATIONAL };
        sig_r = c[0]; sig_g = c[1]; sig_b = c[2];
    }

    if (compact) {
        const char* nav_role = navigation_station != NULL ? station_role_short_name(navigation_station) : "STN";
        const char* dock_role = current_station != NULL ? station_role_short_name(current_station) : "STN";
        const char* bearing_mark = "A";
        if (bearing > 0.12f) {
            bearing_mark = "L";
        } else if (bearing < -0.12f) {
            bearing_mark = "R";
        }

        if (!LOCAL_PLAYER.docked) {
            sdtx_pos(top_text_x, top_row_0);
            sdtx_color3b(PAL_TEXT_PRIMARY);
            {
                /* Use the SESSION callsign (sent over the wire and echoed
                 * back in highscores + remote ship labels) — NOT the local
                 * mining-keypair callsign, which is cryptographic-only and
                 * doesn't match what other players or the death-screen
                 * leaderboard see. */
                const char *cs = net_local_callsign();
                const char *tag = (cs && cs[0] != '\0') ? cs : "SHIP";
                sdtx_puts(tag);
            }

            sdtx_pos(top_text_x, top_row_1);
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_printf("H %d/%d  C %d/%d  ", hull_units, hull_capacity, cargo_units, cargo_capacity);
            sdtx_color3b(sig_r, sig_g, sig_b);
            sdtx_printf("%s %d%%", sig_band, sig_pct);
            if (sig_quality < SIGNAL_BAND_OPERATIONAL) {
                int mine_pct = (int)lroundf(signal_mining_efficiency(sig_quality) * 100.0f);
                int ctrl_pct = (int)lroundf(signal_control_scale(sig_quality) * 100.0f);
                sdtx_printf(" M%d%% CTRL%d%%", mine_pct, ctrl_pct);
            }

            sdtx_pos(top_text_x, top_row_2);
            if (LOCAL_PLAYER.in_dock_range) {
                sdtx_color3b(PAL_SIGNAL_MINT);
                sdtx_puts("DOCK RANGE // E dock");
            } else {
                sdtx_color3b(PAL_NAV_BLUE);
                sdtx_printf("%s %d u // %d %s", nav_role, station_distance, bearing_degrees, bearing_mark);
            }

            sdtx_pos(top_text_x, top_row_3);
            {
                hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
                hud_render_action_compact(&act, dock_role);
            }
        }

        if (!LOCAL_PLAYER.docked && hud_should_draw_message_panel()) {
            /* Subtitle: up to HUD_MSG_LINES wrapped lines, stacked bottom-up. */
            float cell = HUD_CELL * ui_text_zoom();
            int first_line_with_content = -1;
            int last_line_with_content = -1;
            for (int li = 0; li < HUD_MSG_LINES; li++) {
                if (message_lines[li][0] != '\0') {
                    if (first_line_with_content < 0) first_line_with_content = li;
                    last_line_with_content = li;
                }
            }
            int count = (last_line_with_content >= first_line_with_content)
                        ? (last_line_with_content - first_line_with_content + 1) : 0;
            for (int vi = 0; vi < count; vi++) {
                int li = first_line_with_content + vi;
                char full_msg[256];
                if (vi == 0 && message_label[0] != '\0')
                    snprintf(full_msg, sizeof(full_msg), "%s: %s",
                             message_label, message_lines[li]);
                else
                    snprintf(full_msg, sizeof(full_msg), "%s", message_lines[li]);
                float msg_w = (float)strlen(full_msg) * cell;
                float msg_x = (screen_w * 0.5f - msg_w * 0.5f) / cell;
                /* Stack upward so the newest line sits at the panel bottom. */
                float msg_y = (screen_h - 32.0f - (float)(count - 1 - vi) * cell * 1.25f) / cell;
                sdtx_pos(msg_x, msg_y);
                sdtx_color3b(message_r, message_g, message_b);
                sdtx_puts(full_msg);
            }
        }

        draw_station_services(&ui);
        /* Shared post-classify panels also fire in compact mode so a
         * narrow window doesn't lose signal-lost warnings, net status,
         * the hail sigil, etc. (Previously the compact early-return
         * hid them entirely.) */
        hud_draw_shared_panels(screen_w, screen_h, sig_quality, true);
        return;
    }

    sdtx_pos(top_text_x, top_row_0);
    sdtx_color3b(PAL_TEXT_PRIMARY);
    {
        /* SESSION callsign — see compact_top above for why we ignore
         * mining_client_get()->player_callsign here. */
        const char *cs = net_local_callsign();
        const char *fallback = LOCAL_PLAYER.docked ? "DOCK STATUS" : "SHIP STATUS";
        if (cs && cs[0] != '\0')
            sdtx_printf("%s // %s", cs, LOCAL_PLAYER.docked ? "DOCKED" : "FLIGHT");
        else
            sdtx_puts(fallback);
    }

    sdtx_pos(top_text_x, top_row_1);
    sdtx_color3b(PAL_TEXT_SECONDARY);
    if (LOCAL_PLAYER.docked) {
        sdtx_printf("%d %s  H %d/%d  C %d/%d  ",
                    credits, player_current_currency(),
                    hull_units, hull_capacity, cargo_units, cargo_capacity);
    } else {
        sdtx_printf("H %d/%d  C %d/%d  ",
                    hull_units, hull_capacity, cargo_units, cargo_capacity);
    }
    sdtx_color3b(sig_r, sig_g, sig_b);
    sdtx_printf("%s %d%%", sig_band, sig_pct);
    if (sig_quality < SIGNAL_BAND_OPERATIONAL) {
        int mine_eff = (int)lroundf(signal_mining_efficiency(sig_quality) * 100.0f);
        int ctrl_eff = (int)lroundf(signal_control_scale(sig_quality) * 100.0f);
        sdtx_printf("  MINE %d%% CTRL %d%%", mine_eff, ctrl_eff);
    }

    sdtx_pos(top_text_x, top_row_2);
    if (LOCAL_PLAYER.docked && current_station) {
        sdtx_color3b(PAL_SIGNAL_MINT);
        sdtx_printf("%s // docked // E launch", current_station->name);
    } else if (LOCAL_PLAYER.in_dock_range) {
        sdtx_color3b(PAL_SIGNAL_MINT);
        sdtx_puts("In dock range. [E] to dock.");
    } else {
        sdtx_color3b(PAL_NAV_BLUE);
        sdtx_printf("%s %d u // %d deg %s",
            navigation_station != NULL ? navigation_station->name : "Station",
            station_distance,
            bearing_degrees,
            bearing_side);
    }

    sdtx_pos(top_text_x, top_row_3);
    {
        hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
        hud_render_action_wide(&act, current_station);
    }

    /* Subtitle: one clean line, centered at bottom-center.
     *
     * Sell-batch summary takes priority when it has content — the line is
     * rendered as colored segments (total in white/gold, each grade label
     * in its canonical color) so the player can see at a glance which
     * grades just paid out. See shared/mining.h:mining_grade_rgb for the
     * palette. Falls through to the regular message otherwise. */
    bool sell_batch_has_content = false;
    if (g.sell_batch.active || g.sell_batch.display_timer > 0.0f) {
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
            if (g.sell_batch.grade_counts[gi] > 0) { sell_batch_has_content = true; break; }
        }
    }

    if (sell_batch_has_content) {
        const float cell = 8.0f;
        /* Pre-measure the composed line so we can center it. Contract
         * settlements name the paying station and its local currency:
         *   "[ Kepler Yard paid +120 kepler bonds  common x1 ]" */
        char head[128], tail[160];
        const station_t *paid_station = NULL;
        if (!g.sell_batch.mixed_stations &&
            g.sell_batch.station >= 0 &&
            g.sell_batch.station < MAX_STATIONS &&
            station_exists(&g.world.stations[g.sell_batch.station])) {
            paid_station = &g.world.stations[g.sell_batch.station];
        }
        const char *pay_cur = (paid_station && paid_station->currency_name[0])
            ? paid_station->currency_name : "credits";
        int head_len = 0;
        if (g.sell_batch.any_by_contract && paid_station) {
            head_len = snprintf(head, sizeof(head), "[ %s paid +%d %s",
                                paid_station->name, g.sell_batch.total_cr,
                                pay_cur);
        } else if (g.sell_batch.any_by_contract) {
            head_len = snprintf(head, sizeof(head), "[ stations paid +%d credits",
                                g.sell_batch.total_cr);
        } else {
            head_len = snprintf(head, sizeof(head), "[ +%d %s",
                                g.sell_batch.total_cr, pay_cur);
        }
        head_len = (int)strlen(head);
        int tail_off = 0;
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
            int n = g.sell_batch.grade_counts[gi];
            if (n <= 0) continue;
            tail_off += snprintf(tail + tail_off, sizeof(tail) - tail_off,
                                 "  %s x%d", mining_grade_label((mining_grade_t)gi), n);
            if (tail_off >= (int)sizeof(tail) - 8) break;
        }
        int total_cols = head_len + tail_off + 2; /* " ]" */
        float msg_w = (float)total_cols * cell;
        float msg_x0 = screen_w * 0.5f - msg_w * 0.5f;
        float msg_y = screen_h * 0.82f;

        /* Fade during the display tail (last 1s of display_timer). */
        float alpha = 1.0f;
        if (!g.sell_batch.active && g.sell_batch.display_timer < 1.0f)
            alpha = g.sell_batch.display_timer;
        if (alpha < 0.0f) alpha = 0.0f;

        uint8_t total_r = g.sell_batch.any_by_contract ? 255 : 200;
        uint8_t total_g = g.sell_batch.any_by_contract ? 210 : 220;
        uint8_t total_b = g.sell_batch.any_by_contract ?  60 : 230;
        float cur_x = msg_x0;

        /* Payout head: generic station sale or station-named contract pay. */
        sdtx_pos(cur_x / cell, msg_y / cell);
        sdtx_color4b(total_r, total_g, total_b, (uint8_t)(alpha * 255.0f));
        sdtx_puts(head);
        cur_x += (float)head_len * cell;

        /* Per-grade segments, each colored by mining_grade_rgb. */
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
            int n = g.sell_batch.grade_counts[gi];
            if (n <= 0) continue;
            char seg[32];
            int seg_len = snprintf(seg, sizeof(seg), "  %s x%d",
                                   mining_grade_label((mining_grade_t)gi), n);
            uint8_t gr, gg_, gb;
            mining_grade_rgb((mining_grade_t)gi, &gr, &gg_, &gb);
            sdtx_pos(cur_x / cell, msg_y / cell);
            sdtx_color4b(gr, gg_, gb, (uint8_t)(alpha * 255.0f));
            sdtx_puts(seg);
            cur_x += (float)seg_len * cell;
        }

        /* " ]" closer */
        sdtx_pos(cur_x / cell, msg_y / cell);
        sdtx_color4b(total_r, total_g, total_b, (uint8_t)(alpha * 255.0f));
        sdtx_puts(" ]");
    } else if (hud_should_draw_message_panel()) {
        float cell = 8.0f;
        bool is_hull_warn = (message_r == 255 && message_g == 60);
        uint8_t r8 = message_r, g8 = message_g, b8 = message_b;
        if (is_hull_warn) {
            float pulse = 0.5f + 0.5f * sinf((float)sapp_frame_count() * 0.06f);
            r8 = (uint8_t)(message_r * pulse);
            g8 = (uint8_t)(40 + 20 * pulse);
            b8 = (uint8_t)(40 + 10 * pulse);
        }
        /* Count non-empty wrapped lines. Subtitle anchors line 0 at 0.82
         * of screen height; extra lines go BELOW (to avoid collision with
         * docked UI above). Long station hails arrive as 2-4 lines. */
        int count = 0;
        for (int li = 0; li < HUD_MSG_LINES; li++) {
            if (message_lines[li][0] != '\0') count = li + 1;
        }
        float msg_y = screen_h * 0.82f;
        for (int li = 0; li < count; li++) {
            /* 512 (was 256): GCC -Werror=format-truncation traces the
             * message_label (32) + ": " + a wrapped line (HUD_MSG_LINE_CAP)
             * and decides 256 might overflow. Bumping silences the warning
             * without forcing an explicit length check. */
            char line_buf[512];
            if (li == 0 && message_label[0] != '\0')
                snprintf(line_buf, sizeof(line_buf), "%s :: %s",
                         message_label, message_lines[li]);
            else
                snprintf(line_buf, sizeof(line_buf), "%s", message_lines[li]);
            float w = (float)strlen(line_buf) * cell;
            float x = screen_w * 0.5f - w * 0.5f;
            float y = msg_y + (float)li * cell * 1.25f;
            sdtx_pos(x / cell, y / cell);
            sdtx_color3b(r8, g8, b8);
            sdtx_puts(line_buf);
        }
    }

    /* --- [E] context prompt — only when docked. [E] is always LAUNCH. --- */
    if (!g.death_cinematic.active && LOCAL_PLAYER.docked) {
        float ex = ui_text_pos(message_x + 16.0f);
        float ey = ui_text_pos(message_y + message_h + 6.0f);
        sdtx_pos(ex, ey);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_puts("[E] launch");
    }

    /* Shared post-classify panels: connection indicator + alpha banner, nav
     * label, hail sigil, module inspect pane, signal-lost warning.
     * Same call also fires from the compact path so a narrow window
     * doesn't silently lose any of these. */
    hud_draw_shared_panels(screen_w, screen_h, sig_quality, compact);

    draw_station_services(&ui);
}
