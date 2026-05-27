#include <stdarg.h>
#include <stdlib.h>

#include "client.h"
#include "audio.h"
#include "npc.h"
#include "render.h"
#include "rng.h"
#include "net.h"
#include "world_draw.h"
#include "signal_model.h"
#include "input.h"
#include "net_sync.h"
#include "onboarding.h"
#include "avatar.h"
#include "mining_client.h"
#include "base58.h"
#include "manifest.h"
#include "contract_objective.h"
#include "palette.h"


#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

/* SOKOL_IMPL must appear in exactly one .c file.
 * The declaration-only headers are already pulled in by client.h,
 * so we just define the _IMPL macros and re-include for the bodies. */
#define SOKOL_IMPL
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_GL_IMPL
#define SOKOL_DEBUGTEXT_IMPL
#define SOKOL_AUDIO_IMPL
#include "sokol_app.h"
#include "sokol_audio.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "sokol_log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

/* Types, game_t, and HUD constants are in client.h */

game_t g;

static const int MAX_SIM_STEPS_PER_FRAME = 8;

static float station_render_cull_radius(const station_t *st) {
    if (!st) return 0.0f;
    return fmaxf(st->dock_radius, STATION_RING_RADIUS[STATION_NUM_RINGS]) +
        STATION_MODULE_COL_RADIUS + 12.0f;
}

/* Audio mix callback: blends episode video audio + music into SFX output */
static void mix_external_audio(float *buffer, int frames, int channels, void *user) {
    (void)user;
    episode_read_audio(&g.episode, buffer, frames, channels);
    music_read_audio(&g.music, buffer, frames, channels);
}


/* clear_input_state, consume_pressed_input, set_notice: see input.h/c */

/* asteroid_next_tier, asteroid_is_collectible, asteroid_progress_ratio: see asteroid.h/c */

/* commodity_refined_form, commodity_name, commodity_code, commodity_short_name: see commodity.h/c */

/* ship_total_cargo, ship_cargo_amount, station_buy_price, station_inventory_amount: see commodity.h/c */

/* station_at ... navigation_station_ptr: see station_ui.c */
/* station_role_name, station_role_short_name: see station_ui.c */
/* build_station_ui_state, format_station_* helpers: see station_ui.c */
/* station_role_hub_label: see station_ui.c */
/* station_role_color: see station_ui.c */
/* can_afford_upgrade: see economy.h/c */

/* station_dock_anchor, ship_cargo_space: see game_sim.c */

#define NET_INPUT_HEARTBEAT_SEC (1.0f / 6.0f)
#define NET_ACTIVE_INPUT_HEARTBEAT_SEC (1.0f / 12.0f)
#define NET_ACTION_RESEND_SEC (1.0f / 12.0f)
#define NET_ACTION_RETRY_SEC 6.0f
#define NET_CLIENT_METRICS_SEC 15.0f
#define LOCAL_PLAYER_RENDER_CORRECTION_SEC 0.18f
#define LOCAL_PLAYER_RENDER_CORRECTION_LATENCY_SEC 0.34f

static const char *net_action_ack_status_name(uint8_t status) {
    switch (status) {
    case NET_ACTION_ACK_RECEIVED:  return "received";
    case NET_ACTION_ACK_DUPLICATE: return "duplicate";
    case NET_ACTION_ACK_REJECTED:  return "rejected";
    default:                       return "unknown";
    }
}

static const char *net_action_result_status_name(uint8_t status) {
    switch (status) {
    case NET_ACTION_RESULT_OK:       return "ok";
    case NET_ACTION_RESULT_REJECTED: return "rejected";
    case NET_ACTION_RESULT_NOOP:     return "noop";
    default:                         return "unknown";
    }
}

static const char *net_handoff_status_name(uint8_t status) {
    switch (status) {
    case NET_HANDOFF_STATUS_OK: return "ok";
    case NET_HANDOFF_STATUS_REJECTED: return "rejected";
    default: return "unknown";
    }
}

static void on_remote_action_ack(uint16_t action_id, uint16_t input_seq,
                                 uint8_t status, uint8_t action);
static void on_remote_action_result(uint16_t action_id, uint16_t input_seq,
                                    uint8_t status, uint8_t action,
                                    uint32_t server_tick);
static void on_remote_handoff_ticket(uint8_t status, uint8_t source_station,
                                     uint8_t dest_station,
                                     const handoff_ticket_t *ticket);
static void on_remote_handoff_result(uint8_t status, uint8_t reason,
                                     uint8_t dest_station,
                                     const uint8_t ticket_hash[32]);
static void on_remote_latency_sample(uint32_t seq, float rtt_ms,
                                     float server_turnaround_ms);

static bool net_tick_after_u32(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static uint32_t net_input_lead_ticks(void) {
    float rtt = g.net_last_ping_rtt > 0.0f
        ? g.net_last_ping_rtt
        : g.net_last_ack_rtt;
    uint32_t lead = NET_INPUT_LEAD_MIN_TICKS;
    if (rtt > 0.0f) {
        float one_way_ticks = (rtt * 0.5f) / SIM_DT;
        lead = (uint32_t)ceilf(one_way_ticks) + NET_INPUT_LEAD_MIN_TICKS;
    }
    if (lead < NET_INPUT_LEAD_MIN_TICKS) lead = NET_INPUT_LEAD_MIN_TICKS;
    if (lead > NET_INPUT_LEAD_MAX_TICKS) lead = NET_INPUT_LEAD_MAX_TICKS;
    return lead;
}

static uint32_t net_next_input_apply_tick(void) {
    if (g.net_last_server_tick != 0) {
        uint32_t target = g.net_last_server_tick + net_input_lead_ticks();
        if (g.net_prediction_tick_valid) {
            uint32_t predicted_next = g.net_prediction_tick + 1u;
            if (net_tick_after_u32(predicted_next, g.net_last_server_tick) &&
                net_tick_after_u32(target, predicted_next)) {
                target = predicted_next;
            }
        }
        if (!net_tick_after_u32(target, g.net_last_server_tick))
            target = g.net_last_server_tick + 1u;
        return target;
    }
    if (g.net_prediction_tick_valid) return g.net_prediction_tick + 1u;
    return 1u;
}

static void clear_collection_feedback(void) {
    g.collection_feedback_ore = 0.0f;
    g.collection_feedback_fragments = 0;
    g.collection_feedback_timer = 0.0f;
}

static void init_starfield(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        float distance = rand_range(&g.world.rng, 100.0f, WORLD_RADIUS * 2.0f);
        float angle = rand_range(&g.world.rng, 0.0f, TWO_PI_F);
        g.stars[i].pos = v2(cosf(angle) * distance, sinf(angle) * distance);
        g.stars[i].depth = rand_range(&g.world.rng, 0.16f, 0.9f);
        g.stars[i].size = rand_range(&g.world.rng, 0.9f, 2.2f);
        g.stars[i].brightness = rand_range(&g.world.rng, 0.45f, 1.0f);
    }
}

static void reset_world(void) {
    g.local_player_slot = 0;
    world_cleanup(&g.world);
    memset(&g.world, 0, sizeof(g.world));

    if (!g.multiplayer_enabled) {
        /* Singleplayer: use local server as authoritative sim */
        world_cleanup(&g.local_server.world);
        local_server_init(&g.local_server, 0);
        local_server_sync_to_client(&g.local_server);
    } else {
        /* Multiplayer: server manages world, client just predicts */
        world_reset(&g.world);
        player_init_ship(&LOCAL_PLAYER, &g.world);
        LOCAL_PLAYER.connected = true;
    }

    g.tracked_contract = -1;
    g.selected_contract = -1;
    g.target_station = -1;
    g.target_module = -1;
    g.inspect_station = -1;
    g.inspect_module = -1;
    memset(&g.inspect_snapshot, 0, sizeof(g.inspect_snapshot));
    g.inspect_snapshot.target_index = 0xFFu;
    g.inspect_snapshot.module_index = 0xFFu;
    g.inspect_snapshot.home_station = 0xFFu;
    g.inspect_snapshot.dest_station = 0xFFu;
    g.inspect_snapshot_timer = 0.0f;
    g.inspect_was_active = false;
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    g.asteroid_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    g.npc_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(&g.scaffold_interp, 0, sizeof(g.scaffold_interp));
    g.scaffold_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));
    g.cargo_pod_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(&g.player_interp, 0, sizeof(g.player_interp));
    g.player_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(g.scanned_players, 0, sizeof(g.scanned_players));
    reset_station_ring_smoothing();

    /* Seed interp buffers so first frame has valid data */
    memcpy(g.asteroid_interp.curr, g.world.asteroids, sizeof(g.asteroid_interp.curr));
    memcpy(g.asteroid_interp.prev, g.world.asteroids, sizeof(g.asteroid_interp.prev));
    memcpy(g.npc_interp.curr, g.world.npc_ships, sizeof(g.npc_interp.curr));
    memcpy(g.npc_interp.prev, g.world.npc_ships, sizeof(g.npc_interp.prev));
    memcpy(g.scaffold_interp.curr, g.world.scaffolds, sizeof(g.scaffold_interp.curr));
    memcpy(g.scaffold_interp.prev, g.world.scaffolds, sizeof(g.scaffold_interp.prev));
    memcpy(g.cargo_pod_interp.curr, g.world.cargo_pods, sizeof(g.cargo_pod_interp.curr));
    memcpy(g.cargo_pod_interp.prev, g.world.cargo_pods, sizeof(g.cargo_pod_interp.prev));

    g.thrusting = false;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    g.pending_net_action = NET_ACTION_NONE;
    g.pending_net_buy_grade = MINING_GRADE_COUNT; /* sentinel = any */
    g.pending_net_place_station = -1;
    g.pending_net_place_ring    = -1;
    g.pending_net_place_slot    = -1;
    g.net_input_timer = 0.0f;
    g.net_time = 0.0f;
    g.net_input_have_last = false;
    g.net_last_sent_flags = 0;
    g.net_last_sent_mining_target = 0xFFFFu;
    g.net_input_seq = 0;
    g.net_last_server_ack = 0;
    g.net_last_server_tick = 0;
    g.net_input_tick_protocol = false;
    g.net_last_ack_rtt = 0.0f;
    g.net_last_ping_rtt = 0.0f;
    g.net_last_ping_server_turnaround_ms = 0.0f;
    g.net_max_ping_rtt_5s = 0.0f;
    g.net_ping_samples = 0;
    g.net_ping_timer = 0.0f;
    g.net_metrics_timer = 0.0f;
    g.net_metrics_seq = 0;
    g.net_max_ack_rtt_5s = 0.0f;
    g.net_ack_window_elapsed = 0.0f;
    g.net_input_packets_sent = 0;
    g.net_action_packets_sent = 0;
    g.net_action_resend_packets = 0;
    g.net_action_dropped = 0;
    g.net_next_action_id = 1;
    g.net_action_queue_start = 0;
    g.net_action_queue_count = 0;
    memset(&g.net_motion, 0, sizeof(g.net_motion));
    memset(g.net_action_queue, 0, sizeof(g.net_action_queue));
    memset(g.net_input_timing, 0, sizeof(g.net_input_timing));
    net_replay_reset();
    audio_clear_voices(&g.audio);
    clear_collection_feedback();

    /* First-run launch copy is owned by the local guide voice,
     * not by the docked station notice channel. */
}

/* Camera/frustum, asteroid_profile, draw_background, draw_station, draw_ship*,
 * draw_npc_*, draw_beam, draw_remote_players: see world_draw.h/c */

/* draw_ui_scanlines ... draw_hud: see hud.c */
/* draw_station_services: see station_ui.c */


/* is_key_down, is_key_pressed: see input.h/c */

/* ship_forward, ship_muzzle: see ship.h/c */

static void reset_step_feedback(void) {
    LOCAL_PLAYER.hover_asteroid = -1;
    /* Beam prediction: in multiplayer, predict beam START position from
     * local ship state (eliminates 10Hz lag on muzzle position). Server
     * owns beam_active, beam_hit, beam_ineffective, and beam_end — those
     * arrive via WORLD_PLAYERS and are not overwritten here.
     * In singleplayer, local server provides everything each tick. */
    if (g.multiplayer_enabled) {
        /* Only predict the muzzle — server owns everything else */
        if (LOCAL_PLAYER.beam_active) {
            LOCAL_PLAYER.beam_start = ship_muzzle(LOCAL_PLAYER.ship.pos,
                LOCAL_PLAYER.ship.angle, &LOCAL_PLAYER.ship);
        }
    } else {
        LOCAL_PLAYER.beam_active = false;
        LOCAL_PLAYER.beam_hit = false;
    }
    g.thrusting = false;
}

/* sample_input_intent: see input.h/c */

/* Rebuild g.station_manifest_summary from local station manifests.
 * Called once per frame in singleplayer (where the client has direct
 * read access to g.world.stations[s].manifest). In multiplayer the
 * server owns the manifest and pushes NET_MSG_STATION_MANIFEST; the
 * summary is populated via apply_remote_station_manifest in
 * net_sync.c. */
static void refresh_station_manifest_summaries(void) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        /* Zero the row — a station with no manifest units should read zero. */
        memset(&g.station_manifest_summary[s][0][0], 0,
               sizeof(g.station_manifest_summary[s]));
        const station_t *st = &g.world.stations[s];
        if (!st->manifest.units || st->manifest.count == 0) continue;
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            const cargo_unit_t *u = &st->manifest.units[i];
            if (u->commodity >= COMMODITY_COUNT) continue;
            if (u->grade >= MINING_GRADE_COUNT) continue;
            g.station_manifest_summary[s][u->commodity][u->grade]++;
        }
    }
}

static void flush_sell_batch(void) {
    if (!g.sell_batch.active) return;
    /* Stop accumulating; hand off to the HUD render for ~3s so it can
     * draw the totals with per-grade colors in the hint-bar row. The
     * batch payload is preserved so the renderer can name the paying
     * station and currency. */
    g.sell_batch.active = false;
    g.sell_batch.settle_timer = 0.0f;
    g.sell_batch.display_timer = 3.0f;
}

static void step_notice_timer(float dt) {
    if (g.notice_timer > 0.0f) {
        g.notice_timer = fmaxf(0.0f, g.notice_timer - dt);
    }

    if (g.collection_feedback_timer > 0.0f) {
        g.collection_feedback_timer = fmaxf(0.0f, g.collection_feedback_timer - dt);
        if (g.collection_feedback_timer <= 0.0f) {
            clear_collection_feedback();
        }
    }

    if (g.sell_batch.active) {
        g.sell_batch.settle_timer = fmaxf(0.0f, g.sell_batch.settle_timer - dt);
        if (g.sell_batch.settle_timer <= 0.0f) flush_sell_batch();
    }
    if (g.sell_batch.display_timer > 0.0f) {
        g.sell_batch.display_timer = fmaxf(0.0f, g.sell_batch.display_timer - dt);
        if (g.sell_batch.display_timer <= 0.0f) {
            /* Lifetime elapsed — drop the summary. */
            g.sell_batch.total_cr = 0;
            g.sell_batch.any_by_contract = false;
            g.sell_batch.station = -1;
            g.sell_batch.mixed_stations = false;
            for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
                g.sell_batch.grade_counts[gi] = 0;
        }
    }
}

/* No sync_globals_to_world — world_t is the source of truth in single player. */

/* sync_world_to_globals removed — everything reads from g.world directly */

/* ================================================================== */
/* sim_event handlers — one per event type, dispatched via the table   */
/* below. process_sim_events drops to a thin loop: bounds-check the    */
/* event type, look up a handler, call it. Adding a new event = add    */
/* enum value (in shared/types.h, before SIM_EVENT_COUNT), write a     */
/* sim_on_<event> handler, fill its slot in k_sim_event_handlers.      */
/* ================================================================== */
typedef void (*sim_event_handler_fn)(const sim_event_t *ev);

static bool ev_is_local(const sim_event_t *ev) {
    return ev->player_id == g.local_player_slot;
}

static void sim_on_fracture(const sim_event_t *ev) {
    audio_play_fracture(&g.audio, ev->fracture.tier);
    if (ev_is_local(ev)) onboarding_mark_fractured();
}

static void sim_on_mining_tick(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_mining_tick(&g.audio);
}

static void sim_on_dock(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_dock(&g.audio);
    g.screen_shake = fmaxf(g.screen_shake, 3.0f); /* dock clunk */
    g.dock_settle_timer = 1.0f; /* show ship settling before panel */
    int ds = LOCAL_PLAYER.current_station;
    if (ds < SIGNAL_ROOT_STATION_COUNT) {
        g.episode.stations_visited |= (1 << ds);
        if (g.episode.stations_visited == ((1u << SIGNAL_ROOT_STATION_COUNT) - 1u)) /* all relay roots */
            episode_trigger(&g.episode, 1); /* Ep 1: Kepler's Law */
    }
}

static void sim_on_launch(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_launch(&g.audio);
    g.screen_shake = fmaxf(g.screen_shake, 5.0f); /* launch kick */
    episode_trigger(&g.episode, 0); /* Ep 0: First Light */
    if (!g.music.playing && !g.music.loading) music_next_track(&g.music);
}

/* Roll the per-frame sale-fx + hint-bar batch state for one SELL event. */
static void sell_batch_accumulate(const sim_event_t *ev, int total) {
    if (ev->sell.station < 0 || ev->sell.station >= MAX_STATIONS) return;
    if (!station_exists(&g.world.stations[ev->sell.station])) return;
    spawn_sell_fx(&g.world.stations[ev->sell.station].pos, total,
                  (mining_grade_t)ev->sell.grade, ev->sell.by_contract != 0);
    /* If a previous summary is still on-screen (display timer > 0) and
     * we're not already accumulating, this event starts a fresh run —
     * zero the leftover counts first so the new batch isn't contaminated. */
    if (!g.sell_batch.active && g.sell_batch.display_timer > 0.0f) {
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) g.sell_batch.grade_counts[gi] = 0;
        g.sell_batch.total_cr = 0;
        g.sell_batch.any_by_contract = false;
        g.sell_batch.station = -1;
        g.sell_batch.mixed_stations = false;
        g.sell_batch.display_timer = 0.0f;
    }
    if (!g.sell_batch.active && g.sell_batch.display_timer <= 0.0f &&
        g.sell_batch.total_cr == 0) {
        g.sell_batch.station = -1;
        g.sell_batch.mixed_stations = false;
    }
    int grade_idx = (int)ev->sell.grade;
    if (grade_idx >= 0 && grade_idx < MINING_GRADE_COUNT)
        g.sell_batch.grade_counts[grade_idx]++;
    g.sell_batch.total_cr += total;
    if (ev->sell.by_contract) g.sell_batch.any_by_contract = true;
    if (ev->sell.station >= 0 && ev->sell.station < MAX_STATIONS) {
        if (g.sell_batch.station < 0) {
            g.sell_batch.station = ev->sell.station;
        } else if (g.sell_batch.station != ev->sell.station) {
            g.sell_batch.mixed_stations = true;
        }
    }
    g.sell_batch.active = true;
    g.sell_batch.settle_timer = 0.6f;
}

static bool client_hash32_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static void station_hail_label(int station_idx, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) {
        snprintf(out, cap, "Unknown");
        return;
    }
    const station_t *st = &g.world.stations[station_idx];
    if (client_hash32_is_zero(st->station_pubkey)) {
        snprintf(out, cap, "%s", st->name);
        return;
    }
    char id[8];
    mining_callsign_from_pubkey(st->station_pubkey, id);
    snprintf(out, cap, "%s [%s]", st->name, id);
}

static void sim_on_sell(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_sale(&g.audio);
    episode_trigger(&g.episode, 2); /* Ep 2: Furnace — first smelt */
    mining_client_record_strike((mining_grade_t)ev->sell.grade, ev->sell.bonus_cr);
    int total = ev->sell.base_cr + ev->sell.bonus_cr;
    if (total > 0) sell_batch_accumulate(ev, total);
    if (ev->sell.grade >= (uint8_t)MINING_GRADE_RATI &&
        ev->sell.station >= 0 && ev->sell.station < MAX_STATIONS &&
        g.world.stations[ev->sell.station].rati_hail_message[0]) {
        const station_t *st = &g.world.stations[ev->sell.station];
        station_hail_label(ev->sell.station, g.hail_station, sizeof(g.hail_station));
        snprintf(g.hail_message, sizeof(g.hail_message), "%s", st->rati_hail_message);
        g.hail_credits = 0.0f;
        g.hail_station_index = ev->sell.station;
        g.hail_timer = 6.0f;
        set_notice("%s: %s", g.hail_station, g.hail_message);
    }
}

static void sim_on_buy(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    if (ev->buy.commodity >= COMMODITY_COUNT) return;
    const char *unit = "credits";
    if (ev->buy.station >= 0 && ev->buy.station < MAX_STATIONS &&
        g.world.stations[ev->buy.station].currency_name[0]) {
        unit = g.world.stations[ev->buy.station].currency_name;
    }
    set_notice("Bought %u %s for %d %s.",
               (unsigned)(ev->buy.quantity ? ev->buy.quantity : 1),
               commodity_name((commodity_t)ev->buy.commodity),
               ev->buy.cost, unit);
}

static void sim_on_repair(const sim_event_t *ev) {
    if (ev_is_local(ev)) audio_play_repair(&g.audio);
}

static void sim_on_upgrade(const sim_event_t *ev) {
    if (ev_is_local(ev)) audio_play_upgrade(&g.audio, ev->upgrade.upgrade);
}

static void sim_on_damage(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_damage(&g.audio, ev->damage.amount);
    /* Sharper "thunk" overlay so a rock impact reads as a single hit
     * over the broader damage hiss. */
    audio_play_hit_thunk(&g.audio);
    /* Screen shake scales with damage. Tunables chosen so a minor scrape
     * (~2 hp) wiggles a few pixels and a full ramming hit (~30 hp)
     * noticeably jolts. */
    float kick = sqrtf(ev->damage.amount) * 4.0f;
    if (kick > 40.0f) kick = 40.0f;
    if (kick > g.screen_shake) g.screen_shake = kick;
    /* Hit feedback: floating "-N" popup near the receiver's ship + red
     * vignette pulse on the HUD. Both decay independently of the audio. */
    int amount = (int)lroundf(ev->damage.amount);
    if (amount > 0) spawn_damage_fx(&LOCAL_PLAYER.ship.pos, amount);
    g.damage_flash_timer = 0.4f;
    /* Directional indicator — chevron at the screen edge pointing at
     * the threat. Source = (0,0) means "unknown" (legacy / environmental);
     * skip the indicator for those so it doesn't flicker at world origin. */
    if (ev->damage.source_x != 0.0f || ev->damage.source_y != 0.0f) {
        float dx = ev->damage.source_x - LOCAL_PLAYER.ship.pos.x;
        float dy = ev->damage.source_y - LOCAL_PLAYER.ship.pos.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > 1.0f) {
            g.damage_dir_x = dx / d;
            g.damage_dir_y = dy / d;
            g.damage_dir_timer = 1.5f;
        }
    }
}

/* Find or create a scoreboard row for the given attribution token. */
static int scoreboard_row_for_token(const uint8_t token[8], const char *label, bool is_npc) {
    for (int i = 0; i < g.scoreboard.row_count; i++) {
        if (memcmp(g.scoreboard.rows[i].token, token, 8) == 0) return i;
    }
    int cap = (int)(sizeof(g.scoreboard.rows) / sizeof(g.scoreboard.rows[0]));
    if (g.scoreboard.row_count >= cap) return -1;
    int idx = g.scoreboard.row_count++;
    memset(&g.scoreboard.rows[idx], 0, sizeof(g.scoreboard.rows[idx]));
    memcpy(g.scoreboard.rows[idx].token, token, 8);
    if (label && label[0])
        snprintf(g.scoreboard.rows[idx].label, sizeof(g.scoreboard.rows[idx].label), "%s", label);
    g.scoreboard.rows[idx].is_npc = is_npc;
    return idx;
}

static bool token_matches_local(const uint8_t token[8]) {
    /* Zero token = unattributed; never counts as the local player. */
    bool nonzero = false;
    for (int i = 0; i < 8; i++) { if (token[i]) { nonzero = true; break; } }
    if (!nonzero) return false;
    return memcmp(token, g.world.players[g.local_player_slot].session_token, 8) == 0;
}

static void sim_on_npc_kill(const sim_event_t *ev) {
    /* Kill-feed line. Prefer the local player's perspective: if I'm
     * the killer, prepend "You killed"; otherwise show the killer's
     * callsign if we know it (multiplayer player kills NPC). For now
     * we don't have a token-to-callsign cache, so the bare role +
     * cause cover the singleplayer case where the local player is
     * always the killer. */
    const char *role = (ev->npc_kill.npc_role == NPC_ROLE_MINER) ? "Miner"
                     : (ev->npc_kill.npc_role == NPC_ROLE_HAULER) ? "Hauler"
                     : "Tow drone";
    const char *weapon = (ev->npc_kill.cause == DEATH_CAUSE_THROWN_ROCK) ? "thrown rock"
                       : (ev->npc_kill.cause == DEATH_CAUSE_RAM) ? "ramming"
                       : "collision";
    bool you_killed = !g.multiplayer_enabled ||
        (memcmp(ev->npc_kill.killer_token,
                g.world.players[g.local_player_slot].session_token, 8) == 0);
    if (you_killed) {
        snprintf(g.kill_feed_text, sizeof(g.kill_feed_text),
                 "You killed %s with %s", role, weapon);
    } else {
        snprintf(g.kill_feed_text, sizeof(g.kill_feed_text),
                 "%s killed by %s", role, weapon);
    }
    g.kill_feed_timer = 3.0f;

    /* Killer-side feedback: counter, banner, SFX. NPC kills don't have
     * a victim token to attribute against on the scoreboard, but the
     * killer entry still gets credited. */
    if (you_killed) {
        g.kill_count_session++;
        snprintf(g.kill_confirm_text, sizeof(g.kill_confirm_text),
                 "KILL: %s", role);
        g.kill_confirm_timer = 3.0f;
        audio_play_kill_confirm(&g.audio);
        const uint8_t *me = g.world.players[g.local_player_slot].session_token;
        const char *cs = LOCAL_PLAYER.callsign[0] ? LOCAL_PLAYER.callsign : "YOU";
        int row = scoreboard_row_for_token(me, cs, false);
        if (row >= 0) g.scoreboard.rows[row].kills++;
    } else {
        /* Some other player killed the NPC. Credit them on the
         * scoreboard — label is the role since we have no
         * token→callsign map for kills attributed to remotes. */
        int row = scoreboard_row_for_token(ev->npc_kill.killer_token, "Player", false);
        if (row >= 0) g.scoreboard.rows[row].kills++;
    }
}

static void sim_on_contract_complete(const sim_event_t *ev) {
    if (ev->contract_complete.action == CONTRACT_TRACTOR) {
        set_notice("Tractor job complete.");
        episode_trigger(&g.episode, 6); /* Ep 6: Hauler */
    } else if (ev->contract_complete.action == CONTRACT_FRACTURE) {
        set_notice("Fracture job complete.");
    } else if (ev->contract_complete.action == CONTRACT_DELIVERY) {
        set_notice("Delivery job complete.");
    }
}

static void sim_on_scaffold_ready(const sim_event_t *ev) {
    int sidx = ev->scaffold_ready.station;
    int mtype = ev->scaffold_ready.module_type;
    if (sidx < 0 || sidx >= MAX_STATIONS) return;
    set_notice("%s scaffold ready at %s.",
               module_type_name((module_type_t)mtype),
               g.world.stations[sidx].name);
}

static void sim_on_outpost_placed(const sim_event_t *ev) {
    /* Transition from ghost preview to real plan mode: the server just
     * created the planned station at the position where the player
     * locked it. */
    if (!ev_is_local(ev)) return;
    g.plan_target_station = ev->outpost_placed.slot;
    g.placement_target_station = ev->outpost_placed.slot;
}

/* Spawn the 8 shards + cinematic state for a death event. */
static void death_cinematic_spawn(const sim_event_t *ev) {
    float impact_speed = sqrtf(ev->death.vel_x * ev->death.vel_x +
                               ev->death.vel_y * ev->death.vel_y);
    float severity = clampf(impact_speed / 260.0f, 0.8f, 2.4f);
    uint32_t spin_seed = ((uint32_t)ev->death.respawn_station << 24) ^
                         ((uint32_t)ev->death.cause << 16) ^
                         ((uint32_t)ev->death.killer_token[0] << 8) ^
                         client_death_spin_float_bits(impact_speed);
    float spin_dir = client_death_spin_dir(spin_seed);
    g.death_cinematic.active = true;
    g.death_cinematic.phase = 0;
    g.death_cinematic.pos = v2(ev->death.pos_x, ev->death.pos_y);
    g.death_cinematic.vel = v2(ev->death.vel_x, ev->death.vel_y);
    g.death_cinematic.angle = ev->death.angle;
    g.death_cinematic.spin = spin_dir * clampf(3.0f + impact_speed / 45.0f, 5.0f, 16.0f);
    g.death_cinematic.age = 0.0f;
    g.death_cinematic.menu_alpha = 0.0f;
    g.thrusting = false;
    LOCAL_PLAYER.beam_active = false;
    LOCAL_PLAYER.beam_hit = false;
    LOCAL_PLAYER.ship.tractor_active = false;
    g.screen_shake = fmaxf(g.screen_shake, clampf(26.0f + impact_speed * 0.12f, 38.0f, 82.0f));
    for (int s = 0; s < 8; s++) {
        float ang = ((float)s / 8.0f) * 2.0f * PI_F + (float)(s * 13 % 7) * 0.15f;
        float speed = 82.0f + severity * 34.0f + (float)((s * 7 + 3) % 5) * 22.0f;
        g.death_cinematic.fragments[s][0] = 0.0f;
        g.death_cinematic.fragments[s][1] = 0.0f;
        g.death_cinematic.fragments[s][2] = cosf(ang) * speed + ev->death.vel_x * 0.45f;
        g.death_cinematic.fragments[s][3] = sinf(ang) * speed + ev->death.vel_y * 0.45f;
        g.death_cinematic.fragments[s][4] = ang;
        g.death_cinematic.fragments[s][5] = ((float)((s * 19 + 7) % 11) - 5.0f) *
                                            (1.0f + severity * 0.45f);
    }
}

static void sim_on_death(const sim_event_t *ev) {
    /* Scoreboard + kill confirm fire for ANY death we see, not just
     * the local player's. A remote death observed via the broadcast
     * SIM_EVENT_DEATH stream is still useful telemetry for the killer. */
    int vid = ev->player_id;
    const uint8_t *vtoken = NULL;
    const char *vlabel = "Pilot";
    if (vid >= 0 && vid < MAX_PLAYERS) {
        vtoken = g.world.players[vid].session_token;
        if (g.world.players[vid].callsign[0])
            vlabel = g.world.players[vid].callsign;
    }
    bool i_killed_them =
        !ev_is_local(ev) && token_matches_local(ev->death.killer_token);
    if (i_killed_them) {
        g.kill_count_session++;
        snprintf(g.kill_confirm_text, sizeof(g.kill_confirm_text),
                 "KILL: %s", vlabel);
        g.kill_confirm_timer = 3.0f;
        audio_play_kill_confirm(&g.audio);
        const uint8_t *me = g.world.players[g.local_player_slot].session_token;
        const char *cs = LOCAL_PLAYER.callsign[0] ? LOCAL_PLAYER.callsign : "YOU";
        int row = scoreboard_row_for_token(me, cs, false);
        if (row >= 0) g.scoreboard.rows[row].kills++;
    } else if (!ev_is_local(ev)) {
        /* Someone else got the kill — credit them on the scoreboard. */
        bool nonzero = false;
        for (int i = 0; i < 8; i++)
            if (ev->death.killer_token[i]) { nonzero = true; break; }
        if (nonzero) {
            int row = scoreboard_row_for_token(ev->death.killer_token, "Player", false);
            if (row >= 0) g.scoreboard.rows[row].kills++;
        }
    }
    if (vtoken) {
        int vrow = scoreboard_row_for_token(vtoken, vlabel, false);
        if (vrow >= 0) g.scoreboard.rows[vrow].deaths++;
    }

    if (!ev_is_local(ev)) return;
    g.death_count_session++;
    /* In MP the cinematic + payload come via NET_MSG_DEATH (only the
     * victim receives that). The broadcast SIM_EVENT_DEATH stream
     * exists only for kill-confirm + scoreboard attribution and
     * carries zeroed cinematic fields, so don't try to render off it
     * in MP. */
    if (g.multiplayer_enabled) return;
    g.death_ore_mined = ev->death.ore_mined;
    g.death_credits_earned = ev->death.credits_earned;
    g.death_credits_spent = ev->death.credits_spent;
    g.death_asteroids_fractured = ev->death.asteroids_fractured;
    g.death_respawn_station = ev->death.respawn_station;
    g.death_respawn_fee = ev->death.respawn_fee;
    /* Snapshot the wreckage at the death position. The server has
     * already respawned the ship at a station, so we use the position
     * from the death event payload (captured before the move). */
    death_cinematic_spawn(ev);
    /* Legacy timer kept for the auto-fade fallback path. */
    g.death_screen_timer = 0.0f;
    g.death_screen_max = 0.0f;
    /* Force-stop any playing episode, reset state, then trigger death
     * episode so it plays during the cinematic. */
    if (episode_is_active(&g.episode)) episode_skip(&g.episode);
    memset(g.episode.watched, 0, sizeof(g.episode.watched));
    g.episode.stations_visited = 0;
    episode_trigger(&g.episode, 9); /* Ep 9: Death */
    episode_save(&g.episode);
    music_enter_death(&g.music);
}

/* Station hails are operator-authored server state. Tutorial copy lives
 * in onboarding.c; avatar/MOTD fetches are only for profile art/caches. */
static const char *hail_choose_message(int station_idx) {
    if (station_idx < 0 || station_idx >= MAX_STATIONS)
        return "Signal acknowledged.";
    const char *msg = g.world.stations[station_idx].hail_message;
    return msg[0] ? msg : "Signal acknowledged.";
}

static void sim_on_hail_response(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    int hs = ev->hail_response.station;
    if (hs < 0 || hs >= MAX_STATIONS) {
        set_notice("Local scan sweep.");
        return;
    }

    station_hail_label(hs, g.hail_station, sizeof(g.hail_station));
    const char *msg = hail_choose_message(hs);
    snprintf(g.hail_message, sizeof(g.hail_message), "%s", msg);
    float shown_credits = ev->hail_response.credits >= 0.0f ? ev->hail_response.credits : 0.0f;
    g.hail_credits = shown_credits;
    g.hail_station_index = hs;
    g.hail_timer = 6.0f;
    if (g.hail_credits > 0.5f) audio_play_sale(&g.audio);
    if (g.world.stations[hs].station_slug[0])
        avatar_fetch(hs, g.world.stations[hs].station_slug);
    /* Surface the hail through the bottom-right hint bar. Includes
     * the station balance so all the info the old center-screen
     * overlay carried lands there. */
    const char *unit = g.world.stations[hs].currency_name;
    if (!unit[0]) unit = "credits";
    if (ev->hail_response.credits >= 0.0f) {
        set_notice("%s: %s  (balance %d %s)",
                   g.hail_station, g.hail_message,
                   (int)lroundf(shown_credits), unit);
    } else {
        set_notice("%s: %s", g.hail_station, g.hail_message);
    }
    char step[192];
    if (contract_objective_track_contract(ev->hail_response.contract_index,
                                          step, sizeof(step))) {
        set_notice("Tracking: %s", step);
    }
    onboarding_mark_hailed();

}

static void sim_on_module_activated(const sim_event_t *ev) {
    int si = ev->module_activated.station;
    int mi = ev->module_activated.module_idx;
    station_t *act_st = &g.world.stations[si];
    vec2 mpos = module_world_pos_ring(act_st, act_st->modules[mi].ring,
                                       act_st->modules[mi].slot);
    g.commission_timer = 1.5f;
    g.commission_pos = mpos;
    module_color_fn((module_type_t)ev->module_activated.module_type,
                    &g.commission_cr, &g.commission_cg, &g.commission_cb);
    audio_play_commission(&g.audio);
    const char *module_name = module_type_name((module_type_t)ev->module_activated.module_type);
    set_notice("%s online.", module_name);
}

static void sim_on_outpost_activated(const sim_event_t *ev) {
    (void)ev;
    if (!g.episode.watched[4]) episode_trigger(&g.episode, 4); /* Ep 4: Naming */
    audio_play_commission(&g.audio);
}

static void sim_on_npc_spawned(const sim_event_t *ev) {
    /* Ep 5: Drones — first miner at a player outpost */
    if (!g.episode.watched[5] &&
        ev->npc_spawned.role == NPC_ROLE_MINER &&
        ev->npc_spawned.home_station >= SIGNAL_FIRST_OUTPOST_INDEX)
        episode_trigger(&g.episode, 5);
}

static void sim_on_signal_lost(const sim_event_t *ev) {
    if (ev_is_local(ev) && !g.episode.watched[7])
        episode_trigger(&g.episode, 7); /* Ep 7: Dark Sector */
}

static void sim_on_station_connected(const sim_event_t *ev) {
    if (!g.episode.watched[8] && ev->station_connected.connected_count >= 5)
        episode_trigger(&g.episode, 8); /* Ep 8: Every AI Dreams */
}

/* Map order-rejection reason codes to user-visible notices. Reason
 * codes are defined in shared/types.h next to sim_event_t. */
static const char *order_reject_message(uint8_t reason) {
    switch (reason) {
    case ORDER_REJECT_SCAFFOLD_PLACEMENT_NO_SIGNAL:
        return "No signal here -- tow the scaffold back into station coverage.";
    case ORDER_REJECT_SCAFFOLD_PLACEMENT_TOO_CLOSE:
        return "Too close to an existing station -- drop further out toward the fringe.";
    case ORDER_REJECT_SCAFFOLD_PLACEMENT_NEEDS_RELAY:
        return "Only signal-relay scaffolds can found new outposts. Tow this one to an existing station.";
    case ORDER_REJECT_SCAFFOLD_PLACEMENT_NO_SLOT:
        return "No outpost slots available -- every station catalog entry is taken.";
    case ORDER_REJECT_SHIPYARD_NOT_SOLD:    return "This shipyard doesn't sell that scaffold.";
    case ORDER_REJECT_SHIPYARD_QUEUE_FULL:  return "Shipyard queue full -- wait for the next batch to ship.";
    case ORDER_REJECT_SHIPYARD_LOCKED:      return "Tech tree locked -- order the prerequisite module first.";
    case ORDER_REJECT_SHIPYARD_NO_FUNDS:    return "Not enough credits at this station for the order fee.";
    case ORDER_REJECT_SELL_NOT_ACCEPTED:    return "This station has no consumer for that commodity -- try another dock.";
    case ORDER_REJECT_SELL_STATION_BROKE:   return "This station ran out of credit -- sale partial or refused. Try again later.";
    case ORDER_REJECT_SELL_INVENTORY_FULL:  return "This station's hopper is full -- wait for it to consume stock, or try another dock.";
    default:                                return "Order rejected.";
    }
}

static void sim_on_order_rejected(const sim_event_t *ev) {
    set_notice("%s", order_reject_message(ev->order_rejected.reason));
}

/* Dispatch table — designated initializers tie each handler to its
 * enum slot, so reordering enum values doesn't silently misroute. */
static const sim_event_handler_fn k_sim_event_handlers[SIM_EVENT_COUNT] = {
    [SIM_EVENT_FRACTURE]           = sim_on_fracture,
    [SIM_EVENT_MINING_TICK]        = sim_on_mining_tick,
    [SIM_EVENT_DOCK]               = sim_on_dock,
    [SIM_EVENT_LAUNCH]             = sim_on_launch,
    [SIM_EVENT_SELL]               = sim_on_sell,
    [SIM_EVENT_BUY]                = sim_on_buy,
    [SIM_EVENT_REPAIR]             = sim_on_repair,
    [SIM_EVENT_UPGRADE]            = sim_on_upgrade,
    [SIM_EVENT_DAMAGE]             = sim_on_damage,
    [SIM_EVENT_CONTRACT_COMPLETE]  = sim_on_contract_complete,
    [SIM_EVENT_SCAFFOLD_READY]     = sim_on_scaffold_ready,
    [SIM_EVENT_OUTPOST_PLACED]     = sim_on_outpost_placed,
    [SIM_EVENT_DEATH]              = sim_on_death,
    [SIM_EVENT_HAIL_RESPONSE]      = sim_on_hail_response,
    [SIM_EVENT_MODULE_ACTIVATED]   = sim_on_module_activated,
    [SIM_EVENT_OUTPOST_ACTIVATED]  = sim_on_outpost_activated,
    [SIM_EVENT_NPC_SPAWNED]        = sim_on_npc_spawned,
    [SIM_EVENT_SIGNAL_LOST]        = sim_on_signal_lost,
    [SIM_EVENT_STATION_CONNECTED]  = sim_on_station_connected,
    [SIM_EVENT_ORDER_REJECTED]     = sim_on_order_rejected,
    [SIM_EVENT_NPC_KILL]           = sim_on_npc_kill,
};

void process_sim_events(const sim_events_t *events) {
    for (int i = 0; i < events->count; i++) {
        const sim_event_t *ev = &events->events[i];
        if ((unsigned)ev->type >= SIM_EVENT_COUNT) continue;
        sim_event_handler_fn h = k_sim_event_handlers[ev->type];
        if (h) h(ev);
    }
}

static void onboarding_per_frame(void) {
    if (g.onboarding.complete) return;
    /* Tractor milestone: detect fragment pickup via towed_count */
    if (!g.onboarding.tractored && LOCAL_PLAYER.ship.towed_count > 0)
        onboarding_mark_tractored();
}

static void episode_per_frame(void) {
    if (episode_is_active(&g.episode)) return;

    /* Ep 3: Scaffold — currently towing a scaffold */
    if (!g.episode.watched[3] && LOCAL_PLAYER.ship.towed_scaffold >= 0)
        episode_trigger(&g.episode, 3);

    /* Ep 4, 5, 7, 8 are now event-driven (see process_events) */
}

static void sim_step(float dt) {
    reset_step_feedback();
    audio_step(&g.audio, dt);

    /* Advance world time locally in multiplayer (server doesn't send it).
     * Ring rotations are authoritative server-side; client prediction
     * integrates omega each frame, while net_sync eases any phase correction
     * from the latest station-identity snapshot instead of snapping. */
    if (g.multiplayer_enabled) {
        g.world.time += dt;
        step_remote_station_rings(dt);
    }

    /* Commission flash countdown */
    if (g.commission_timer > 0.0f)
        g.commission_timer = fmaxf(0.0f, g.commission_timer - dt);
    if (g.hail_timer > 0.0f)
        g.hail_timer = fmaxf(0.0f, g.hail_timer - dt);
    if (g.inspect_snapshot_timer > 0.0f) {
        g.inspect_snapshot_timer = fmaxf(0.0f, g.inspect_snapshot_timer - dt);
        if (g.inspect_snapshot_timer == 0.0f) {
            /* Linger fully decayed — clear so the next idle frame's
             * apply_*_inspect_snapshot doesn't see was_active stuck
             * true and re-bump the timer. */
            g.inspect_snapshot.target_type = INSPECT_TARGET_NONE;
            g.inspect_snapshot.target_index = 0xFFu;
        }
    }
    /* Station chain-event heartbeats: ~1s decay so a single delta reads
     * as one short pulse, but consecutive ticks compound (clamped 1.0). */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (g.station_heartbeat[s] > 0.0f)
            g.station_heartbeat[s] = fmaxf(0.0f, g.station_heartbeat[s] - dt);
    }
    if (g.hail_ping_timer > 0.0f) {
        g.hail_ping_timer += dt;
        if (g.hail_ping_timer > 8.00f) g.hail_ping_timer = 0.0f; /* HAIL_PING_LIFECYCLE */
    }
    {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        float target = signal_visual_saturation(sig);
        if (!g.signal_visual_saturation_initialized) {
            g.signal_visual_saturation = target;
            g.signal_visual_saturation_initialized = true;
        } else {
            float k = 1.0f - expf(-dt / 0.35f);
            g.signal_visual_saturation += (target - g.signal_visual_saturation) * k;
        }
    }
    if (g.outpost_lock_timer > 0.0f)
        g.outpost_lock_timer = fmaxf(0.0f, g.outpost_lock_timer - dt);
    mining_client_tick(dt);

    /* Smoothed fog intensity. Tracks 1 - hull/max_hull, but eases in
     * (slow ramp up) and out (faster ramp down) so the vignette rolls
     * cinematically. Forced to 1.0 while the death cinematic is active. */
    {
        float frac = LOCAL_PLAYER.ship.hull / fmaxf(1.0f, ship_max_hull(&LOCAL_PLAYER.ship));
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float target = 1.0f - frac;
        if (g.death_cinematic.active) target = 1.0f;
        if (g.death_screen_timer > 0.0f) target = 1.0f;
        /* In = ~1s ease, out = ~2s ease (recovery is more languid) */
        float k = (target > g.fog_intensity) ? (1.0f - expf(-dt / 1.0f))
                                              : (1.0f - expf(-dt / 2.0f));
        g.fog_intensity += (target - g.fog_intensity) * k;
    }

    /* Death cinematic.
     *   Phase 0 (drift): wreckage tumbles, fog rolls in, and the HUD
     *      flashes SYSTEM CRITICAL while the world fades down.
     *      After DEATH_CINEMATIC_WORLD_PHASE_SEC the cinematic
     *      auto-advances to phase 1 — no input required.
     *   Phase 1 (stats): the stat menu fades in over the wreckage.
     *      Pressing E (after the menu has settled) releases the
     *      cinematic and lets the same E press fall through to the
     *      normal docked-launch handler.
     *   Phase 2 (closing): cinematic.active is false but menu_alpha
     *      decays back toward 0 so the stat screen visibly disappears. */
    if (g.death_cinematic.active) {
        g.thrusting = false;
        LOCAL_PLAYER.beam_active = false;
        LOCAL_PLAYER.beam_hit = false;
        LOCAL_PLAYER.ship.tractor_active = false;
        if (g.death_cinematic.phase == 0 &&
            g.death_cinematic.age >= DEATH_CINEMATIC_WORLD_PHASE_SEC) {
            g.death_cinematic.phase = 1;
        }
        if (g.death_cinematic.phase == 1 && g.death_cinematic.menu_alpha >= 0.85f
            && is_key_pressed(SAPP_KEYCODE_E)) {
            g.death_cinematic.active = false;
            g.death_cinematic.phase = 2;
            music_exit_death(&g.music);
            /* Don't consume the E press — normal input below picks it
             * up and launches the (now-respawned) docked ship. */
        }
    }

    /* Menu alpha — eases in during phase 1, eases out otherwise.
     * Lives outside the active block so it keeps fading after release. */
    {
        float menu_target = (g.death_cinematic.active && g.death_cinematic.phase >= 1) ? 1.0f : 0.0f;
        float mk = 1.0f - expf(-dt / 0.5f);
        g.death_cinematic.menu_alpha += (menu_target - g.death_cinematic.menu_alpha) * mk;
        if (g.death_cinematic.menu_alpha < 0.005f) g.death_cinematic.menu_alpha = 0.0f;
    }

    if (g.death_cinematic.active) {
        g.death_cinematic.age += dt;
        /* Wreckage hull drift with mild damping */
        g.death_cinematic.pos.x += g.death_cinematic.vel.x * dt;
        g.death_cinematic.pos.y += g.death_cinematic.vel.y * dt;
        float damp = expf(-dt * 0.4f);
        g.death_cinematic.vel.x *= damp;
        g.death_cinematic.vel.y *= damp;
        g.death_cinematic.angle += g.death_cinematic.spin * dt;
        g.death_cinematic.spin *= expf(-dt * 0.2f);
        /* Shards drift outward with damping */
        for (int i = 0; i < 8; i++) {
            float *f = g.death_cinematic.fragments[i];
            f[0] += f[2] * dt;
            f[1] += f[3] * dt;
            f[2] *= expf(-dt * 0.6f);
            f[3] *= expf(-dt * 0.6f);
            f[4] += f[5] * dt;
            f[5] *= expf(-dt * 0.3f);
        }

        /* Force-stop the ship at the station so it doesn't drift while
         * we're showing the wreckage. (Server has it docked but cargo /
         * hull updates may still arrive — we keep velocity zero locally.) */
        LOCAL_PLAYER.ship.vel = v2(0.0f, 0.0f);

        /* Keep episode video and music running during death cinematic */
        episode_update(&g.episode, dt);
        music_update(&g.music, dt);

        consume_pressed_input();
        return;
    }

    /* Legacy death screen countdown (unused while cinematic is active) */
    if (g.death_screen_timer > 0.0f) {
        g.death_screen_timer = fmaxf(0.0f, g.death_screen_timer - dt);
        consume_pressed_input();
        return;
    }

    input_intent_t intent = sample_input_intent();

    /* Reset the docked view to the first visible station panel. */
    if (LOCAL_PLAYER.docked && !g.was_docked) {
        const station_t* st = current_station_ptr();
        g.station_view = station_panel_first_visible(st);
        g.selected_contract = -1; /* fresh dock — no carryover selection */
        reset_trade_session_rows(LOCAL_PLAYER.current_station);
        /* Clear blueprint pip if we docked at the blueprint station */
        if (st) {
            if (g.nav_pip_is_blueprint) {
                float d = sqrtf(v2_dist_sq(st->pos, g.nav_pip_pos));
                if (d < 200.0f) {
                    g.nav_pip_is_blueprint = false;
                    g.nav_pip_pos = st->pos;
                }
            } else {
                g.nav_pip_active = true;
                g.nav_pip_pos = st->pos;
            }
        }
    }
    submit_input(&intent, dt);

    /* Version mismatch: reload once to get matching client.
     * Only reload if we haven't already tried (check ?v= in URL).
     * deploy-client runs before deploy-server, so the new client
     * is on CDN by the time the new server sends its hash. */
    if (g.multiplayer_enabled && net_is_connected()) {
        const char *srv = net_server_hash();
#ifdef GIT_HASH
        const char *cli = GIT_HASH;
#else
        const char *cli = "dev";
#endif
        if (srv[0] != '\0' && strcmp(cli, "dev") != 0 && strcmp(cli, srv) != 0) {
#ifdef __EMSCRIPTEN__
            /* Only reload once, preserving smoke/server/singleplayer params. */
            int already_tried = emscripten_run_script_int(
                "new URLSearchParams(location.search).has('v') ? 1 : 0");
            if (!already_tried) {
                emscripten_run_script(
                    "(() => {"
                    "  const u = new URL(location.href);"
                    "  u.searchParams.set('v', Date.now().toString());"
                    "  location.replace(u.pathname + '?' + u.searchParams.toString() + u.hash);"
                    "})()");
            }
#endif
        }
    }

    /* Advance interpolation timers (both modes) */
    g.asteroid_interp.t += dt / fmaxf(g.asteroid_interp.interval, 0.01f);
    g.npc_interp.t += dt / fmaxf(g.npc_interp.interval, 0.01f);
    g.scaffold_interp.t += dt / fmaxf(g.scaffold_interp.interval, 0.01f);
    g.cargo_pod_interp.t += dt / fmaxf(g.cargo_pod_interp.interval, 0.01f);
    g.player_interp.t += dt / fmaxf(g.player_interp.interval, 0.01f);

    /* Thrust flame: local input for manual, server input for autopilot.
     * In SP the local server mirrors input.thrust; in MP the PLAYER_STATE
     * flags carry bit0=thrust which we decode into g.server_thrusting. */
    if (LOCAL_PLAYER.autopilot_mode) {
        g.thrusting = g.server_thrusting && !LOCAL_PLAYER.docked;
    } else {
        g.thrusting = (intent.thrust > 0.0f) && !LOCAL_PLAYER.docked;
    }

    /* Play audio + trigger UI from sim events (both local and multiplayer) */
    process_sim_events(&g.world.events);
    g.world.events.count = 0;  /* consume — don't replay on next sim step */

    /* Detect state transitions for music/episode triggers (works in both modes).
     * Must run BEFORE was_docked is updated to detect the transition. */
    if (g.was_docked && !LOCAL_PLAYER.docked) {
        /* Just launched */
        fprintf(stderr, "LAUNCH: triggering ep 0 + music\n");
        episode_trigger(&g.episode, 0);
        if (!g.music.playing && !g.music.loading)
            music_next_track(&g.music);
    }
    if (!g.was_docked && LOCAL_PLAYER.docked) {
        /* Just docked */
        int ds = LOCAL_PLAYER.current_station;
        if (ds < SIGNAL_ROOT_STATION_COUNT) {
            g.episode.stations_visited |= (1 << ds);
            if (g.episode.stations_visited == ((1u << SIGNAL_ROOT_STATION_COUNT) - 1u))
                episode_trigger(&g.episode, 1);
        }
    }

    /* Autopilot disengage detection — notify player why it stopped */
    if (g.was_autopilot && !LOCAL_PLAYER.autopilot_mode) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig < SIGNAL_BAND_OPERATIONAL)
            set_notice("Autopilot disengaged -- weak signal.");
    }
    g.was_autopilot = LOCAL_PLAYER.autopilot_mode;

    /* Death: handled by SIM_EVENT_DEATH (singleplayer) or NET_MSG_DEATH (multiplayer) */

    /* Update was_docked AFTER transition checks */
    g.was_docked = LOCAL_PLAYER.docked;

    onboarding_per_frame();
    episode_per_frame();
    episode_update(&g.episode, dt);
    music_update(&g.music, dt);
    {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        music_update_signal(&g.music, sig);
    }

    /* X = self-destruct handled via input intent (works in both modes) */

    /* ESC dismisses episode popup */
    if (episode_is_active(&g.episode) && g.input.key_pressed[SAPP_KEYCODE_ESCAPE])
        episode_skip(&g.episode);

    /* Music controls: P = pause/unpause, [ = prev, ] = next */
    if (g.input.key_pressed[SAPP_KEYCODE_P]) {
        if (g.music.playing)
            g.music.paused ? music_resume(&g.music) : music_pause(&g.music);
        else
            music_next_track(&g.music);
    }
    if (g.input.key_pressed[SAPP_KEYCODE_RIGHT_BRACKET] && g.music.playing)
        music_next_track(&g.music);
    if (g.input.key_pressed[SAPP_KEYCODE_LEFT_BRACKET] && g.music.playing)
        music_prev_track(&g.music);

    step_notice_timer(dt);
    update_sell_fx(dt);
    update_damage_fx(dt);
    if (g.damage_dir_timer > 0.0f) {
        g.damage_dir_timer -= dt;
        if (g.damage_dir_timer < 0.0f) g.damage_dir_timer = 0.0f;
    }
    if (g.kill_feed_timer > 0.0f) {
        g.kill_feed_timer -= dt;
        if (g.kill_feed_timer < 0.0f) {
            g.kill_feed_timer = 0.0f;
            g.kill_feed_text[0] = '\0';
        }
    }
    if (g.kill_confirm_timer > 0.0f) {
        g.kill_confirm_timer -= dt;
        if (g.kill_confirm_timer < 0.0f) {
            g.kill_confirm_timer = 0.0f;
            g.kill_confirm_text[0] = '\0';
        }
    }
    /* Tractor-lock pulse: 0->positive towed_count edge plays a short
     * "lock" tone so the player has audio feedback the grab took. */
    {
        static int prev_towed = 0;
        int now_towed = LOCAL_PLAYER.ship.towed_count;
        if (now_towed > prev_towed) audio_play_tractor_lock(&g.audio);
        prev_towed = now_towed;
    }
    /* Tab while undocked toggles the session scoreboard. Docked Tab is
     * already taken (cycle station tabs); the gating mirrors that. */
    if (!LOCAL_PLAYER.docked && g.input.key_pressed[SAPP_KEYCODE_TAB]) {
        g.scoreboard.show = !g.scoreboard.show;
    }
    if (g.action_predict_timer > 0.0f)
        g.action_predict_timer = fmaxf(0.0f, g.action_predict_timer - dt);
    if (g.dock_settle_timer > 0.0f)
        g.dock_settle_timer = fmaxf(0.0f, g.dock_settle_timer - dt);

    consume_pressed_input();
}

/* on_player_join ... sync_local_player_slot_from_network: see net_sync.h/c */

static void init(void) {
    memset(&g, 0, sizeof(g));
    g.world.rng = 0xC0FFEE12u;

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    sgl_setup(&(sgl_desc_t){
        .logger.func = slog_func,
    });

    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_oric(),
        .logger.func = slog_func,
    });

    audio_init(&g.audio);
    g.audio.mix_callback = mix_external_audio;
    g.audio.mix_callback_user = NULL;

    episode_init(&g.episode);
    episode_load(&g.episode);
    music_init(&g.music);
    avatar_init();
    hull_fog_init();


    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value = (sg_color){ PAL_F_VOID_CLEAR, 1.0f };

    init_starfield();
    reset_world();

    /* Load (or first-run-generate) the persistent Ed25519 player identity.
     * Layer A.1 of #479 — purely client-side for now: no network or save
     * coupling, just persistence + HUD display. */
    if (identity_load_or_generate(&g.identity)) {
        base58_encode(g.identity.pubkey,
                      SIGNAL_CRYPTO_PUBKEY_BYTES,
                      g.identity_pub_b58,
                      sizeof(g.identity_pub_b58));
    }

    onboarding_load();
    mining_client_init();
    /* Bind to whatever session token the local server seeded — the
     * multiplayer connect path will rebind to the real one once the
     * WS handshake completes. */
    mining_client_set_session_token(g.world.players[g.local_player_slot].session_token);

    /* --- Multiplayer: auto-connect if server URL is available --- */
    {
        const char* server_url = NULL;
#ifdef __EMSCRIPTEN__
        server_url = emscripten_run_script_string(
            "(() => {"
            "  const p = new URLSearchParams(window.location.search);"
            "  if (p.has('singleplayer')) return '';"
            "  return p.get('server') || window.SIGNAL_SERVER || '';"
            "})()");
#else
        /* Native: check SIGNAL_SERVER environment variable or command line */
        server_url = getenv("SIGNAL_SERVER");
#endif
        if (server_url && server_url[0] != '\0') {
            NetCallbacks cbs = {0};
            cbs.on_join = on_player_join;
            cbs.on_leave = on_player_leave;
            cbs.on_players_begin = begin_player_state_batch;
            cbs.on_state = apply_remote_player_state;
            cbs.on_asteroids = apply_remote_asteroids;
            cbs.on_npcs = apply_remote_npcs;
            cbs.on_stations = apply_remote_stations;
            cbs.on_station_identity = apply_remote_station_identity;
            cbs.on_station_diag = apply_remote_station_diag;
            cbs.on_scaffolds = apply_remote_scaffolds;
            cbs.on_cargo_pods = apply_remote_cargo_pods;
            cbs.on_hail_response = apply_remote_hail_response;
            cbs.on_player_ship = apply_remote_player_ship;
            cbs.on_contracts = apply_remote_contracts;
            cbs.on_player_known_contracts = apply_remote_player_known_contracts;
            cbs.on_delivery_ledger = apply_remote_delivery_ledger;
            cbs.on_death = on_remote_death;
            cbs.on_world_time = on_remote_world_time;
            cbs.on_events = apply_remote_events;
            cbs.on_signal_channel = apply_remote_signal_channel;
            cbs.on_station_manifest = apply_remote_station_manifest;
            cbs.on_player_manifest = apply_remote_player_manifest;
            cbs.on_cargo_receipt_bundle = apply_remote_cargo_receipt_bundle;
            cbs.on_station_ingots = apply_remote_station_ingots;
            cbs.on_hold_ingots = apply_remote_hold_ingots;
            cbs.on_inspect_snapshot = apply_remote_inspect_snapshot;
            cbs.on_highscores = apply_remote_highscores;
            cbs.on_action_ack = on_remote_action_ack;
            cbs.on_action_result = on_remote_action_result;
            cbs.on_latency_sample = on_remote_latency_sample;
            cbs.on_handoff_ticket = on_remote_handoff_ticket;
            cbs.on_handoff_result = on_remote_handoff_result;
            /* Layer A.2 of #479 — hand the persistent pubkey to net.c
             * BEFORE net_init so the first WebSocket on_open already
             * has it ready to send via NET_MSG_REGISTER_PUBKEY. */
            net_set_identity_pubkey(g.identity.pubkey);
            /* Layer A.3 of #479 — install the secret so the client can
             * sign state-changing actions on the NET_MSG_SIGNED_ACTION
             * channel. The secret never leaves the process. */
            net_set_identity_secret(g.identity.secret);
            g.multiplayer_enabled = net_init(server_url, &cbs);
            if (g.multiplayer_enabled) {
                /* Deactivate the local server — the remote server is authoritative.
                 * The local server was started by reset_world() before we knew
                 * multiplayer was available. Clear dynamic state seeded by that
                 * bootstrap world so active-only remote snapshots cannot leave
                 * local-only asteroid/NPC ghosts behind. */
                g.local_server.active = false;
                reset_remote_dynamic_sync();
            }
        }
    }
}


/* on_player_join ... sync_local_player_slot_from_network: see net_sync.h/c */

static void render_world(void) {
    /* Guard against Safari's NaN-on-audio-resume frame (the same one
     * ui_window_width handles). Unguarded NaN here propagates through
     * set_camera_bounds into cam_right - cam_left, then into
     * draw_callsigns/draw_npc_chatter's sdtx_canvas call, which
     * asserts !isnan(w). See hud.h ui_safe_positive comment. */
    float win_w = ui_safe_positive(sapp_widthf(), 1280.0f);
    float win_h = ui_safe_positive(sapp_heightf(), 720.0f);
    float half_w = win_w * 0.5f;
    float half_h = win_h * 0.5f;
    /* Camera modes:
     *   1. Death cinematic — anchor to wreckage, mild damping
     *   2. Station encounter — lock the station to one side of the screen
     *      (left or right) based on which way the player approached
     *   3. Free flight — DEADZONE camera. The ship moves freely inside
     *      a center deadzone. When it hits the edge of the deadzone the
     *      camera latches to that edge and follows. Sustained high-speed
     *      motion lazily recenters the camera onto the ship. */
    if (!g.camera_initialized) {
        g.camera_pos = LOCAL_PLAYER.ship.pos;
        g.camera_initialized = true;
        g.boost_zoom = 1.0f;
        g.boost_center_blend = 0.0f;
    }

    /* Boost camera: target zoom-in + center-on-ship while SHIFT is
     * held, smooth return on release. Inverse of the hail ping. */
    {
        float cdt = (float)sapp_frame_duration();
        if (cdt <= 0.0f) cdt = 1.0f / 60.0f;
        if (cdt > 0.1f) cdt = 0.1f;
        bool boosting = (g.input.key_down[SAPP_KEYCODE_LEFT_SHIFT]
                       || g.input.key_down[SAPP_KEYCODE_RIGHT_SHIFT])
                       && !LOCAL_PLAYER.docked && !g.death_cinematic.active;
        float target_zoom  = boosting ? 0.82f : 1.0f;
        float target_blend = boosting ? 1.0f  : 0.0f;
        /* Slower ease-in than ease-out so the zoom feels like it locks
         * on gradually while active, and release is equally gentle. */
        float kz = 1.0f - expf(-cdt / 0.9f);
        float kb = 1.0f - expf(-cdt / 0.7f);
        g.boost_zoom         += (target_zoom  - g.boost_zoom)         * kz;
        g.boost_center_blend += (target_blend - g.boost_center_blend) * kb;
    }

    {
        /* Camera lerp uses real frame duration so the smoothing rate is
         * frame-rate independent. The previous hardcoded 1/60 caused the
         * camera to over- or under-step at non-60Hz refresh rates, which
         * showed up as a "jump" when the ship hit the deadzone edge. */
        float dt = (float)sapp_frame_duration();
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f; /* clamp on tab-resume / hitches */
        const station_t *anchor_station = NULL;
        if (LOCAL_PLAYER.docked && LOCAL_PLAYER.current_station >= 0
            && LOCAL_PLAYER.current_station < MAX_STATIONS)
            anchor_station = &g.world.stations[LOCAL_PLAYER.current_station];
        else if (LOCAL_PLAYER.nearby_station >= 0
                 && LOCAL_PLAYER.nearby_station < MAX_STATIONS
                 && station_exists(&g.world.stations[LOCAL_PLAYER.nearby_station]))
            anchor_station = &g.world.stations[LOCAL_PLAYER.nearby_station];

        if (g.death_cinematic.active) {
            /* (1) DEATH — snap camera onto the wreckage. We use the
             * cinematic age as a "first-frame" check: at age 0 the
             * camera was potentially at the now-respawned ship's
             * station, so jump straight to the wreckage. After that
             * just track it directly (the wreckage drifts mildly). */
            g.camera_pos = g.death_cinematic.pos;
            g.camera_drift_timer = 0.0f;
            g.camera_station_index = -1;
        } else if (anchor_station) {
            /* (2) STATION — lock the station to L/R + T/B side of the
             * screen depending on which way the ship approached from.
             * Latch the side once and hold so the camera doesn't flip
             * if the player drifts past the station mid-encounter. */
            int station_idx = (LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station);
            if (g.camera_station_index != station_idx) {
                g.camera_station_index = station_idx;
                /* Snapshot side based on the ship→station vector at first
                 * sight. If ship is left of station, station goes on the
                 * right side of the screen (1/4 from right edge). */
                g.camera_station_side = (LOCAL_PLAYER.ship.pos.x <= anchor_station->pos.x) ? +1 : -1;
                g.camera_station_v_side = (LOCAL_PLAYER.ship.pos.y <= anchor_station->pos.y) ? +1 : -1;
            }
            /* Position station ~25% from the chosen edge. Target camera
             * is shifted away from the station by that offset. */
            float anchor_x_off = -(float)g.camera_station_side * half_w * 0.45f;
            float anchor_y_off = -(float)g.camera_station_v_side * half_h * 0.30f;
            vec2 target = v2(anchor_station->pos.x + anchor_x_off,
                             anchor_station->pos.y + anchor_y_off);
            float k = 1.0f - expf(-1.6f * dt);
            g.camera_pos.x += (target.x - g.camera_pos.x) * k;
            g.camera_pos.y += (target.y - g.camera_pos.y) * k;
            g.camera_drift_timer = 0.0f;
        } else {
            /* (3) FREE FLIGHT — deadzone camera. */
            g.camera_station_index = -1;
            vec2 ship = LOCAL_PLAYER.ship.pos;
            float dz_x = half_w * 0.45f;  /* deadzone half-width */
            float dz_y = half_h * 0.40f;  /* deadzone half-height */
            float dx = ship.x - g.camera_pos.x;
            float dy = ship.y - g.camera_pos.y;

            /* Edge follow: when the ship pushes past the deadzone edge,
             * SMOOTHLY approach the latched position (ship sitting on
             * the boundary) rather than snapping. */
            float k = 1.0f - expf(-6.0f * dt);
            if (dx >  dz_x) {
                float target_x = ship.x - dz_x;
                g.camera_pos.x += (target_x - g.camera_pos.x) * k;
            }
            if (dx < -dz_x) {
                float target_x = ship.x + dz_x;
                g.camera_pos.x += (target_x - g.camera_pos.x) * k;
            }
            if (dy >  dz_y) {
                float target_y = ship.y - dz_y;
                g.camera_pos.y += (target_y - g.camera_pos.y) * k;
            }
            if (dy < -dz_y) {
                float target_y = ship.y + dz_y;
                g.camera_pos.y += (target_y - g.camera_pos.y) * k;
            }

            /* Lazy recenter: if the ship has been outside the deadzone
             * for >1.5s while moving fast, slowly drift the camera
             * toward the ship so the framing eventually catches up. */
            bool outside_dz = fabsf(dx) >= dz_x * 0.98f || fabsf(dy) >= dz_y * 0.98f;
            float speed_sq = LOCAL_PLAYER.ship.vel.x * LOCAL_PLAYER.ship.vel.x
                           + LOCAL_PLAYER.ship.vel.y * LOCAL_PLAYER.ship.vel.y;
            if (outside_dz && speed_sq > 90.0f * 90.0f) {
                g.camera_drift_timer += dt;
            } else {
                g.camera_drift_timer = 0.0f;
            }
            if (g.camera_drift_timer > 1.5f) {
                float drift_k = 1.0f - expf(-0.8f * dt);
                g.camera_pos.x += (ship.x - g.camera_pos.x) * drift_k;
                g.camera_pos.y += (ship.y - g.camera_pos.y) * drift_k;
            }

            /* Boost centering: scale the ease-to-ship by boost_center_blend
             * so as the player holds SHIFT the deadzone softly dissolves
             * and the ship slides into dead-center. Releases the same way. */
            if (g.boost_center_blend > 0.001f) {
                float kcen = (1.0f - expf(-3.0f * dt)) * g.boost_center_blend;
                g.camera_pos.x += (ship.x - g.camera_pos.x) * kcen;
                g.camera_pos.y += (ship.y - g.camera_pos.y) * kcen;
            }
        }

        /* Scan-target framing: bias the camera toward the midpoint
         * between the local player and the scanned NPC so both stay
         * visible. Skipped during the death cinematic. Strength is
         * full while actively scanning; after release it tracks the
         * linger timer normalized to [0,1] so the framing eases back
         * to neutral as the panel fades out. */
        if (!g.death_cinematic.active
            && g.inspect_snapshot.target_type == INSPECT_TARGET_NPC
            && g.inspect_snapshot_timer > 0.0f
            && g.inspect_snapshot.target_index < MAX_NPC_SHIPS) {
            int idx = (int)g.inspect_snapshot.target_index;
            const npc_ship_t *tn = &g.world.npc_ships[idx];
            if (tn->active) {
                vec2 mid = v2(0.5f * (LOCAL_PLAYER.ship.pos.x + tn->ship.pos.x),
                              0.5f * (LOCAL_PLAYER.ship.pos.y + tn->ship.pos.y));
                float strength = g.inspect_was_active
                                 ? 1.0f
                                 : (g.inspect_snapshot_timer < 1.0f
                                    ? g.inspect_snapshot_timer : 1.0f);
                float k = (1.0f - expf(-2.5f * dt)) * 0.6f * strength;
                g.camera_pos.x += (mid.x - g.camera_pos.x) * k;
                g.camera_pos.y += (mid.y - g.camera_pos.y) * k;
            }
        }
    }
    vec2 camera = g.camera_pos;

    /* Screen shake: chaotic offset overlay that decays exponentially.
     * Uses a pair of phase-shifted sines so the motion is biaxial and
     * doesn't trace a clean line. */
    if (g.screen_shake > 0.05f) {
        g.screen_shake_seed += 1.0f;
        float t = g.screen_shake_seed;
        float ox = sinf(t * 1.71f) * cosf(t * 0.93f);
        float oy = sinf(t * 1.13f + 1.7f) * cosf(t * 1.41f);
        camera.x += ox * g.screen_shake;
        camera.y += oy * g.screen_shake;
        /* Decay ~20%/frame at 60fps → ~0.5s tail at moderate hits */
        g.screen_shake *= 0.82f;
    } else {
        g.screen_shake = 0.0f;
    }

    /* Composed camera zoom:
     *   ping_zoom   — widens on H-hail, slow drift back.
     *   boost_zoom  — tightens while SHIFT boost is held, smooth release.
     * They multiply cleanly: hail while boosting gives a ~1.0x "net" view. */
    float ping_zoom = hail_ping_camera_zoom();
    float total_zoom = ping_zoom * g.boost_zoom;
    set_camera_bounds(camera, half_w * total_zoom, half_h * total_zoom);
    world_draw_begin_frame();

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(cam_left(), cam_right(), cam_top(), cam_bottom(), -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    render_set_saturation(world_signal_visual_base_saturation());
    render_set_min_saturation(0.0f);
    render_set_saturation_sampler(world_signal_visual_saturation_at, NULL);

    draw_background(camera);
    draw_signal_borders();

    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t* st = &g.world.stations[i];
        if (!station_exists(st) && !st->scaffold) continue;
        if (!on_screen(st->pos.x, st->pos.y, station_render_cull_radius(st))) continue;
        bool is_current = LOCAL_PLAYER.docked && (i == LOCAL_PLAYER.current_station);
        bool is_nearby = (!LOCAL_PLAYER.docked) && (i == LOCAL_PLAYER.nearby_station);
        draw_station(st, is_current, is_nearby);
    }
    /* Outpost placement preview */
    /* Module commissioning flash */
    if (g.commission_timer > 0.0f) {
        float t = g.commission_timer / 1.5f; /* 1.0 → 0.0 */
        float flash_r = (1.0f - t) * 80.0f + 30.0f; /* expanding ring */
        float alpha = t * 0.8f; /* fading out */
        draw_circle_filled(g.commission_pos, flash_r * 0.4f, 12,
            g.commission_cr, g.commission_cg, g.commission_cb, alpha * 0.3f);
        draw_circle_outline(g.commission_pos, flash_r, 20,
            g.commission_cr, g.commission_cg, g.commission_cb, alpha);
        draw_circle_outline(g.commission_pos, flash_r * 0.6f, 16,
            g.commission_cr * 0.8f, g.commission_cg * 0.8f, g.commission_cb * 0.8f, alpha * 0.6f);
    }

    draw_asteroids();
    draw_cargo_pods();
    draw_scaffolds();
    draw_shipyard_intake_beams();
    draw_placement_reticle();
    draw_beam();
    draw_collision_sparks();
    draw_ship_tractor_field();
    draw_towed_tethers();
    draw_scaffold_tether();
    draw_ship();
    draw_death_wreckage();
    draw_npc_ships();
    draw_remote_players(); /* Multiplayer: remote player ships */
    draw_callsigns();      /* Readable sdtx labels above local + remote ships */
    draw_npc_chatter();    /* Short radio one-liners near NPC sprites (#291) */
    draw_sell_fx();        /* +$N payout popups floating above stations */
    draw_damage_fx();      /* -N hit popups floating above the receiver's ship */
    draw_autopilot_path(); /* Dotted line showing A* path ahead */
    draw_tracked_contract_highlight();  /* Pulsing ring on the current contract's next objective */
    draw_compass_ring();   /* Navigation compass around player ship */

    /* Ring trusses and modules render ON TOP of ships */
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t* st = &g.world.stations[i];
        if (!station_exists(st)) continue;
        if (!on_screen(st->pos.x, st->pos.y, station_render_cull_radius(st))) continue;
        bool is_current = LOCAL_PLAYER.docked && (i == LOCAL_PLAYER.current_station);
        bool is_nearby = (!LOCAL_PLAYER.docked) && (i == LOCAL_PLAYER.nearby_station);
        draw_station_rings(st, is_current, is_nearby);
    }
    draw_hopper_tractors();

    /* Module target highlight + info panel */
    if (g.target_station >= 0 && g.target_module >= 0) {
        const station_t *tst = station_at(g.target_station);
        if (tst && g.target_module < tst->module_count) {
            const station_module_t *tm = &tst->modules[g.target_module];
            vec2 mp = module_world_pos_ring(tst, tm->ring, tm->slot);
            /* Pulsing highlight ring around targeted module */
            float tp = 0.6f + 0.4f * sinf(g.world.time * 5.0f);
            draw_circle_outline(mp, 50.0f, 20, 0.3f, 1.0f, 0.7f, tp * 0.7f);
            draw_circle_outline(mp, 52.0f, 20, 0.3f, 1.0f, 0.7f, tp * 0.3f);
            /* Tractor line from ship to target */
            draw_segment(LOCAL_PLAYER.ship.pos, mp, 0.2f, 0.8f, 1.0f, tp * 0.3f);
            /* Info text near module (world-space debugtext) */
            float screen_w = ui_screen_width();
            float screen_h = ui_screen_height();
            sdtx_canvas(screen_w, screen_h);
            sdtx_origin(0, 0);
            /* Convert world pos to screen pos */
            vec2 cam = LOCAL_PLAYER.ship.pos;
            float sx = (mp.x - cam.x) + screen_w * 0.5f;
            float sy = (mp.y - cam.y) + screen_h * 0.5f;
            float cell = 8.0f;
            sdtx_color3b(130, 255, 200);
            sdtx_pos((sx + 60.0f) / cell, (sy - 20.0f) / cell);
            sdtx_puts(module_type_name(tm->type));
            /* Module-specific info line */
            sdtx_color3b(180, 190, 210);
            sdtx_pos((sx + 60.0f) / cell, (sy - 8.0f) / cell);
            /* Module-specific info + action hint */
            commodity_t sell_c = -1;
            switch (tm->type) {
                case MODULE_FURNACE: {
                    /* Furnace output follows the instance commodity tag. */
                    sell_c = module_instance_output(tm);
                    break;
                }
                case MODULE_FRAME_PRESS: sell_c = COMMODITY_FRAME; break;
                case MODULE_LASER_FAB:   sell_c = COMMODITY_LASER_MODULE; break;
                case MODULE_TRACTOR_FAB: sell_c = COMMODITY_TRACTOR_MODULE; break;
                default: break;
            }
            if ((int)sell_c >= 0) {
                int stock = (int)lroundf(tst->_inventory_cache[sell_c]);
                int price = (int)lroundf(station_sell_price(tst, sell_c));
                if (stock > 0)
                    sdtx_printf("Stock %d // dock to trade @ %d cr", stock, price);
                else
                    sdtx_puts("Out of stock");
            } else switch (tm->type) {
                case MODULE_REPAIR_BAY:
                    sdtx_puts("Dock to repair hull");
                    break;
                case MODULE_DOCK:
                    sdtx_puts("[E] dock");
                    break;
                case MODULE_SIGNAL_RELAY:
                    sdtx_printf("Signal range %.0f", tst->signal_range);
                    break;
                default:
                    break;
            }

            station_flow_diag_t flow = station_module_flow_diag_view(
                tst, g.target_module, g.multiplayer_enabled && net_is_connected());
            if (flow != STATION_FLOW_DIAG_NONE) {
                if (flow == STATION_FLOW_DIAG_RUNNING)
                    sdtx_color3b(120, 230, 180);
                else if (flow == STATION_FLOW_DIAG_SLOW_FEED)
                    sdtx_color3b(245, 210, 115);
                else
                    sdtx_color3b(255, 135, 120);
                sdtx_pos((sx + 60.0f) / cell, (sy + 4.0f) / cell);
                sdtx_printf("flow: %s", station_flow_diag_label(flow));
            }
        }
    }

    /* Tracked contract highlight is owned by draw_tracked_contract_highlight()
     * (called earlier in the world pass). The legacy duplicate that lived
     * here drew a giant dock-radius ring around stations on TRACTOR contracts
     * and a stale target_index ring for FRACTURE — both wrong. Single source. */

    /* Hail ping — draw last so the ring sits on top of stations and
     * modules, hard to miss. */
    draw_hail_ping();

    render_set_saturation_sampler(NULL, NULL);
    render_set_min_saturation(0.0f);
    render_set_saturation(1.0f);
}

static void render_ui(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    render_set_screen_space(screen_w, screen_h);

    draw_hud_panels();
    draw_hud();

    /* Episode video popup — bottom-right corner, doesn't block gameplay */
    if (episode_is_active(&g.episode)) {
        episode_render(&g.episode, screen_w, screen_h);
    }

    /* Music track display — bottom-left, fades after 5s */
    if (g.music.playing && (g.music.current_track >= 0 || g.music.death_mode)) {
        float mt = g.music.track_display_timer;
        float music_alpha = 1.0f;
        if (mt < 0.5f) music_alpha = mt / 0.5f;              /* fade in */
        else if (mt > 5.0f) music_alpha = 1.0f - (mt - 5.0f) / 2.0f; /* fade out */
        if (g.music.paused) music_alpha = 1.0f;               /* always visible when paused */
        if (music_alpha > 0.01f) {
            const music_track_info_t *track = g.music.death_mode
                ? music_get_death_info(g.music.death_track)
                : music_get_info(g.music.current_track);
            if (track) {
                sdtx_canvas(screen_w, screen_h);
                sdtx_origin(0.0f, 0.0f);
                float cell = 8.0f;
                float row = (screen_h - 16.0f) / cell;
                uint8_t a = (uint8_t)(music_alpha * 255.0f);
                /* Right-align: measure total width */
                char label[128];
                /* `[/]` is the pause/resume hotkey (see sample_music in
                 * input.c). Old label trailed a literal " M" — vestige
                 * from when M was the pause key, now removed. */
                if (g.music.paused)
                    snprintf(label, sizeof(label), "PAUSED %s  [/]", track->title);
                else
                    snprintf(label, sizeof(label), "%s  [/]", track->title);
                float tw = (float)strlen(label) * cell;
                sdtx_pos((screen_w - tw - 12.0f) / cell, row);
                if (g.music.paused) {
                    sdtx_color4b(120, 100, 70, a);
                    sdtx_puts("PAUSED ");
                }
                sdtx_color4b(100, 90, 65, a);
                sdtx_puts(track->title);
                sdtx_color4b(60, 55, 45, a);
                sdtx_puts("  [/]");
            }
        }
    }
}

/* interpolate_world_for_render: see net_sync.h/c */

static void step_local_player_render_offset(float dt) {
    if (!g.multiplayer_enabled || g.local_server.active ||
        g.death_cinematic.active || LOCAL_PLAYER.docked) {
        g.local_player_render_offset = v2(0.0f, 0.0f);
        return;
    }

    float len_sq = v2_len_sq(g.local_player_render_offset);
    if (len_sq < 0.01f) {
        g.local_player_render_offset = v2(0.0f, 0.0f);
        return;
    }

    /* Decay the visual correction over a few frames. Simulation state stays
     * corrected; only the rendered ship/camera eases out the packet snap. */
    float correction_sec = lerpf(LOCAL_PLAYER_RENDER_CORRECTION_SEC,
                                 LOCAL_PLAYER_RENDER_CORRECTION_LATENCY_SEC,
                                 net_prediction_latency_blend());
    float keep = expf(-dt / correction_sec);
    g.local_player_render_offset =
        v2_scale(g.local_player_render_offset, keep);
}

static void render_frame(void) {
    interpolate_world_for_render();
    float frame_dt = (float)sapp_frame_duration();
    if (frame_dt <= 0.0f) frame_dt = 1.0f / 60.0f;
    if (frame_dt > 0.1f) frame_dt = 0.1f;
    step_local_player_render_offset(frame_dt);

    /* Damage vignette back wave — sgl-queued before world geometry so
     * world content draws on top. Front wave is queued later by the HUD
     * pass. Set screen-space ortho explicitly; render_world will overwrite
     * with its world-space matrices. */
    {
        float screen_w = ui_screen_width();
        float screen_h = ui_screen_height();
        render_set_screen_space(screen_w, screen_h);
        draw_hull_fog_back();
    }

    vec2 saved_ship_pos = LOCAL_PLAYER.ship.pos;
    bool apply_visual_offset = v2_len_sq(g.local_player_render_offset) > 0.01f;
    if (apply_visual_offset)
        LOCAL_PLAYER.ship.pos = v2_add(LOCAL_PLAYER.ship.pos,
                                       g.local_player_render_offset);
    render_world();
    if (apply_visual_offset)
        LOCAL_PLAYER.ship.pos = saved_ship_pos;
    render_ui();

    sg_begin_pass(&(sg_pass){
        .action = g.pass_action,
        .swapchain = sglue_swapchain(),
    });
    sgl_draw();
    sdtx_draw();
    sg_end_pass();
    sg_commit();
}

static void advance_simulation_frame(float frame_dt) {
    g.runtime.accumulator += frame_dt;

    int sim_steps = 0;
    while ((g.runtime.accumulator >= SIM_DT) && (sim_steps < MAX_SIM_STEPS_PER_FRAME)) {
        sim_step(SIM_DT);
        g.runtime.accumulator -= SIM_DT;
        sim_steps++;
    }

    if (g.runtime.accumulator >= SIM_DT) {
        g.runtime.accumulator = 0.0f;
    }
}

/* Exported for the JS music player — returns 0.0-1.0 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_signal_strength(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_signal_visual_saturation(void) {
    if (!g.signal_visual_saturation_initialized)
        return signal_visual_saturation(get_signal_strength());
    return g.signal_visual_saturation;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_signal_visual_base_saturation(void) {
    return world_signal_visual_base_saturation();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_signal_visual_cue_saturation(void) {
    return world_signal_visual_cue_saturation();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_remote_player_scanned(int player_id) {
    return net_remote_player_scanned(player_id) ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int reset_net_motion_telemetry(void) {
    memset(&g.net_motion, 0, sizeof(g.net_motion));
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_correction(void) {
    return g.net_motion.max_correction_run;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_applied_correction(void) {
    return g.net_motion.max_applied_correction_run;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_velocity_error(void) {
    return g.net_motion.max_velocity_error_run;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_last_ack_rtt_ms(void) {
    return g.net_last_ack_rtt * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_last_ping_rtt_ms(void) {
    return g.net_last_ping_rtt * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_last_ack_gap_ms(void) {
    float ack_ms = g.net_last_ack_rtt * 1000.0f;
    float ping_ms = g.net_last_ping_rtt * 1000.0f;
    if (ack_ms <= 0.0f || ping_ms <= 0.0f) return 0.0f;
    return (ack_ms > ping_ms) ? (ack_ms - ping_ms) : 0.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_last_ping_server_turnaround_ms(void) {
    return g.net_last_ping_server_turnaround_ms;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_ping_rtt_ms(void) {
    return g.net_max_ping_rtt_5s * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_ping_samples(void) {
    return (int)g.net_ping_samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_samples(void) {
    return (int)g.net_motion.total_samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_deferred_samples(void) {
    return (int)g.net_motion.total_deferred_samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_replayed_samples(void) {
    return (int)g.net_motion.total_replayed_samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_replayed_frames(void) {
    return (int)g.net_motion.total_replayed_frames;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_player_interval_ms(void) {
    return g.net_motion.packet_interval * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_player_interval_ms(void) {
    return g.net_motion.max_packet_interval_run * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_player_jitter_ms(void) {
    return g.net_motion.max_packet_jitter_run * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_ack_rtt_ms(void) {
    return g.net_motion.max_ack_rtt_run * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_current_render_offset(void) {
    return v2_len(g.local_player_render_offset);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_render_offset(void) {
    return g.net_motion.max_render_offset_run;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_player_batches(void) {
    return (int)g.net_motion.total_player_batches;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_snap_samples(void) {
    return (int)g.net_motion.total_snap_samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_lerp_samples(void) {
    return (int)g.net_motion.total_lerp_samples;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_total_input_acks(void) {
    return (int)g.net_motion.total_input_acks;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_tick_skew(void) {
    return (int)g.net_motion.tick_skew;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_max_tick_skew_abs(void) {
    return (int)g.net_motion.max_tick_skew_abs;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_replay_depth(void) {
    return (int)g.net_replay_count;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_unacked_inputs(void) {
    return (int)((uint16_t)(g.net_input_seq - g.net_last_server_ack));
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
int signal_smoke_remote_towable_interp_check(void) {
    bool saved_local_server_active = g.local_server.active;
    scaffold_t saved_world_scaffolds[MAX_SCAFFOLDS];
    scaffold_t saved_scaffold_prev[MAX_SCAFFOLDS];
    scaffold_t saved_scaffold_curr[MAX_SCAFFOLDS];
    cargo_pod_t saved_world_cargo_pods[MAX_CARGO_PODS];
    cargo_pod_t saved_cargo_pod_prev[MAX_CARGO_PODS];
    cargo_pod_t saved_cargo_pod_curr[MAX_CARGO_PODS];
    float saved_scaffold_t = g.scaffold_interp.t;
    float saved_scaffold_interval = g.scaffold_interp.interval;
    float saved_cargo_pod_t = g.cargo_pod_interp.t;
    float saved_cargo_pod_interval = g.cargo_pod_interp.interval;

    memcpy(saved_world_scaffolds, g.world.scaffolds, sizeof(saved_world_scaffolds));
    memcpy(saved_scaffold_prev, g.scaffold_interp.prev, sizeof(saved_scaffold_prev));
    memcpy(saved_scaffold_curr, g.scaffold_interp.curr, sizeof(saved_scaffold_curr));
    memcpy(saved_world_cargo_pods, g.world.cargo_pods, sizeof(saved_world_cargo_pods));
    memcpy(saved_cargo_pod_prev, g.cargo_pod_interp.prev, sizeof(saved_cargo_pod_prev));
    memcpy(saved_cargo_pod_curr, g.cargo_pod_interp.curr, sizeof(saved_cargo_pod_curr));

    g.local_server.active = false;
    memset(g.world.scaffolds, 0, sizeof(g.world.scaffolds));
    memset(&g.scaffold_interp, 0, sizeof(g.scaffold_interp));
    g.scaffold_interp.interval = 0.1f;
    memset(g.world.cargo_pods, 0, sizeof(g.world.cargo_pods));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));
    g.cargo_pod_interp.interval = 0.1f;

    NetScaffoldState scaffold = {
        .index = 3,
        .state = SCAFFOLD_LOOSE,
        .module_type = MODULE_DOCK,
        .owner = -1,
        .pos_x = 0.0f,
        .pos_y = 0.0f,
        .vel_x = 100.0f,
        .vel_y = 0.0f,
        .radius = 30.0f,
        .build_amount = 0.0f,
    };
    apply_remote_scaffolds(&scaffold, 1);
    g.scaffold_interp.t = 0.1f / fmaxf(g.scaffold_interp.interval, 0.001f);
    interpolate_world_for_render();
    float scaffold_first_x = g.world.scaffolds[3].pos.x;
    scaffold.pos_x = 100.0f;
    scaffold.vel_x = 0.0f;
    apply_remote_scaffolds(&scaffold, 1);
    g.scaffold_interp.t = 0.05f / fmaxf(g.scaffold_interp.interval, 0.001f);
    interpolate_world_for_render();
    float scaffold_blended_x = g.world.scaffolds[3].pos.x;

    NetCargoPodState pod = {
        .index = 5,
        .kind = CARGO_POD_CARGO,
        .commodity = COMMODITY_FERRITE_INGOT,
        .towed_by = -1,
        .pos_x = 0.0f,
        .pos_y = 0.0f,
        .vel_x = 100.0f,
        .vel_y = 0.0f,
        .radius = 18.0f,
        .rotation = 0.0f,
        .quantity = 12,
    };
    apply_remote_cargo_pods(&pod, 1);
    g.cargo_pod_interp.t = 0.1f / fmaxf(g.cargo_pod_interp.interval, 0.001f);
    interpolate_world_for_render();
    float pod_first_x = g.world.cargo_pods[5].pos.x;
    pod.pos_x = 100.0f;
    pod.vel_x = 0.0f;
    apply_remote_cargo_pods(&pod, 1);
    g.cargo_pod_interp.t = 0.05f / fmaxf(g.cargo_pod_interp.interval, 0.001f);
    interpolate_world_for_render();
    float pod_blended_x = g.world.cargo_pods[5].pos.x;

    int ok = scaffold_first_x > 9.0f && scaffold_first_x < 11.5f &&
             scaffold_blended_x > scaffold_first_x &&
             scaffold_blended_x < 95.0f &&
             pod_first_x > 9.0f && pod_first_x < 11.5f &&
             pod_blended_x > pod_first_x &&
             pod_blended_x < 95.0f;

    memcpy(g.world.scaffolds, saved_world_scaffolds, sizeof(saved_world_scaffolds));
    memcpy(g.scaffold_interp.prev, saved_scaffold_prev, sizeof(saved_scaffold_prev));
    memcpy(g.scaffold_interp.curr, saved_scaffold_curr, sizeof(saved_scaffold_curr));
    memcpy(g.world.cargo_pods, saved_world_cargo_pods, sizeof(saved_world_cargo_pods));
    memcpy(g.cargo_pod_interp.prev, saved_cargo_pod_prev, sizeof(saved_cargo_pod_prev));
    memcpy(g.cargo_pod_interp.curr, saved_cargo_pod_curr, sizeof(saved_cargo_pod_curr));
    g.scaffold_interp.t = saved_scaffold_t;
    g.scaffold_interp.interval = saved_scaffold_interval;
    g.cargo_pod_interp.t = saved_cargo_pod_t;
    g.cargo_pod_interp.interval = saved_cargo_pod_interval;
    g.local_server.active = saved_local_server_active;
    return ok ? 1 : 0;
}
#endif

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_action_queue_depth(void) {
    return (int)g.net_action_queue_count;
}

static net_action_queue_item_t *net_action_queue_at(int offset) {
    int index = ((int)g.net_action_queue_start + offset) % NET_ACTION_QUEUE_CAP;
    return &g.net_action_queue[index];
}

static void net_action_queue_pop_front(void) {
    if (g.net_action_queue_count == 0) return;
    memset(net_action_queue_at(0), 0, sizeof(g.net_action_queue[0]));
    g.net_action_queue_start =
        (uint8_t)((g.net_action_queue_start + 1u) % NET_ACTION_QUEUE_CAP);
    g.net_action_queue_count--;
    if (g.net_action_queue_count == 0) g.net_action_queue_start = 0;
}

static void net_action_queue_remove_at(int offset) {
    if (offset < 0 || offset >= (int)g.net_action_queue_count) return;
    for (int i = offset; i < (int)g.net_action_queue_count - 1; i++) {
        *net_action_queue_at(i) = *net_action_queue_at(i + 1);
    }
    memset(net_action_queue_at((int)g.net_action_queue_count - 1),
           0, sizeof(g.net_action_queue[0]));
    g.net_action_queue_count--;
    if (g.net_action_queue_count == 0) g.net_action_queue_start = 0;
}

static int net_action_queue_find(uint16_t action_id) {
    if (action_id == 0) return -1;
    for (int i = 0; i < (int)g.net_action_queue_count; i++) {
        const net_action_queue_item_t *item = net_action_queue_at(i);
        if (item->active && item->action_id == action_id) return i;
    }
    return -1;
}

static uint16_t net_next_action_id_alloc(void) {
    uint16_t id = g.net_next_action_id++;
    if (g.net_next_action_id == 0) g.net_next_action_id = 1;
    if (id == 0) id = g.net_next_action_id++;
    return id;
}

static void net_action_queue_update(float dt) {
    if (g.net_action_queue_count == 0) return;
    net_action_queue_item_t *item = net_action_queue_at(0);
    item->age += dt;
    if (item->resend_timer > 0.0f) {
        item->resend_timer -= dt;
        if (item->resend_timer < 0.0f) item->resend_timer = 0.0f;
    }

    while (g.net_action_queue_count > 0) {
        item = net_action_queue_at(0);
        if (item->age > NET_ACTION_RETRY_SEC) {
            g.net_action_dropped++;
            net_action_queue_pop_front();
            continue;
        }
        break;
    }
}

static void on_remote_action_ack(uint16_t action_id, uint16_t input_seq,
                                 uint8_t status, uint8_t action) {
    /* ACTION_ACK is an immediate transport/dedupe receipt. Authoritative
     * input age is measured from WORLD_PLAYERS input_seq_ack instead. */
    int offset = net_action_queue_find(action_id);
    if (offset >= 0) net_action_queue_remove_at(offset);
    fprintf(stderr,
            "[net-action] ack id=%u seq=%u action=%u status=%s q=%u predict=%.2f docked=%d balance=%.0f\n",
            (unsigned)action_id, (unsigned)input_seq, (unsigned)action,
            net_action_ack_status_name(status),
            (unsigned)g.net_action_queue_count,
            g.action_predict_timer,
            LOCAL_PLAYER.docked ? 1 : 0,
            g.station_balance);
    if (status == NET_ACTION_ACK_REJECTED && action != NET_ACTION_NONE)
        g.action_predict_timer = 0.0f;
}

static void on_remote_action_result(uint16_t action_id, uint16_t input_seq,
                                    uint8_t status, uint8_t action,
                                    uint32_t server_tick) {
    fprintf(stderr,
            "[net-action] result id=%u seq=%u action=%u status=%s server_tick=%u predict=%.2f docked=%d station=%d balance=%.0f q=%u\n",
            (unsigned)action_id, (unsigned)input_seq, (unsigned)action,
            net_action_result_status_name(status),
            (unsigned)server_tick,
            g.action_predict_timer,
            LOCAL_PLAYER.docked ? 1 : 0,
            LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station,
            g.station_balance,
            (unsigned)g.net_action_queue_count);
    if (server_tick != 0) {
        g.net_last_server_tick = server_tick;
        if (!g.net_prediction_tick_valid) {
            g.net_prediction_tick = server_tick;
            g.net_prediction_tick_valid = true;
        }
    }
    g.action_predict_timer = 0.0f;
}

static void on_remote_handoff_ticket(uint8_t status, uint8_t source_station,
                                     uint8_t dest_station,
                                     const handoff_ticket_t *ticket) {
    g.net_handoff_last_status = status;
    g.net_handoff_source_station = source_station;
    g.net_handoff_dest_station = dest_station;
    g.net_handoff_ticket_valid = false;
    memset(&g.net_handoff_ticket, 0, sizeof(g.net_handoff_ticket));
    if (status == NET_HANDOFF_STATUS_OK && ticket) {
        g.net_handoff_ticket = *ticket;
        g.net_handoff_ticket_valid = true;
    }
    fprintf(stderr,
            "[net-handoff] ticket source=%u dest=%u status=%s valid=%d\n",
            (unsigned)source_station, (unsigned)dest_station,
            net_handoff_status_name(status),
            g.net_handoff_ticket_valid ? 1 : 0);
}

static void on_remote_handoff_result(uint8_t status, uint8_t reason,
                                     uint8_t dest_station,
                                     const uint8_t ticket_hash[32]) {
    (void)ticket_hash;
    g.net_handoff_last_status = status;
    g.net_handoff_last_reason = reason;
    fprintf(stderr,
            "[net-handoff] result dest=%u status=%s reason=%u\n",
            (unsigned)dest_station, net_handoff_status_name(status),
            (unsigned)reason);
    if (status == NET_HANDOFF_STATUS_OK)
        g.net_handoff_ticket_valid = false;
}

static void on_remote_latency_sample(uint32_t seq, float rtt_ms,
                                     float server_turnaround_ms) {
    (void)seq;
    if (rtt_ms < 0.0f || rtt_ms > 30000.0f) return;
    g.net_last_ping_rtt = rtt_ms / 1000.0f;
    g.net_last_ping_server_turnaround_ms = server_turnaround_ms;
    if (g.net_last_ping_rtt > g.net_max_ping_rtt_5s)
        g.net_max_ping_rtt_5s = g.net_last_ping_rtt;
    g.net_ping_samples++;
}

static void net_action_queue_push(uint8_t action, uint8_t buy_grade,
                                  int8_t place_station, int8_t place_ring,
                                  int8_t place_slot) {
    if (action == NET_ACTION_NONE) return;
    if (g.net_action_queue_count >= NET_ACTION_QUEUE_CAP) {
        g.net_action_dropped++;
        return;
    }

    uint16_t id = net_next_action_id_alloc();

    net_action_queue_item_t *item =
        net_action_queue_at((int)g.net_action_queue_count);
    memset(item, 0, sizeof(*item));
    item->active = true;
    item->action = action;
    item->buy_grade = buy_grade;
    item->place_station = place_station;
    item->place_ring = place_ring;
    item->place_slot = place_slot;
    item->action_id = id;
    g.net_action_queue_count++;
}

static bool net_action_queue_head_first_send(void) {
    if (g.net_action_queue_count == 0) return false;
    const net_action_queue_item_t *item = net_action_queue_at(0);
    return item->active && item->send_count == 0;
}

static bool action_receipt_filters(uint8_t action, uint8_t buy_grade,
                                   commodity_t *out_commodity,
                                   mining_grade_t *out_grade) {
    if (out_commodity) *out_commodity = COMMODITY_COUNT;
    if (out_grade) *out_grade = MINING_GRADE_COUNT;
    if (action == NET_ACTION_SELL_CARGO) return true;
    if (action >= NET_ACTION_DELIVER_COMMODITY &&
        action < NET_ACTION_DELIVER_COMMODITY + COMMODITY_COUNT) {
        if (out_commodity)
            *out_commodity = (commodity_t)(action - NET_ACTION_DELIVER_COMMODITY);
        if (out_grade && buy_grade < MINING_GRADE_COUNT)
            *out_grade = (mining_grade_t)buy_grade;
        return true;
    }
    return false;
}

static void net_present_receipt_chains_for_action(uint8_t action,
                                                  uint8_t buy_grade) {
    commodity_t commodity_filter = COMMODITY_COUNT;
    mining_grade_t grade_filter = MINING_GRADE_COUNT;
    if (!action_receipt_filters(action, buy_grade,
                                &commodity_filter, &grade_filter)) {
        return;
    }

    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS)
        return;
    ship_t *ship = &g.world.players[g.local_player_slot].ship;
    if (!ship || !ship->manifest.units) return;
    const ship_receipts_t *receipts = ship_get_receipts_const(ship);
    if (!receipts || !receipts->chains) return;

    uint16_t count = ship->manifest.count;
    if (count > receipts->count) count = receipts->count;
    for (uint16_t i = 0; i < count; i++) {
        const cargo_unit_t *unit = &ship->manifest.units[i];
        const cargo_receipt_chain_t *chain = &receipts->chains[i];
        if (commodity_filter < COMMODITY_COUNT &&
            unit->commodity != (uint8_t)commodity_filter) {
            continue;
        }
        if (grade_filter < MINING_GRADE_COUNT &&
            unit->grade != (uint8_t)grade_filter) {
            continue;
        }
        if (!chain || chain->len == 0 ||
            chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
            continue;
        }
        if (cargo_receipt_chain_verify(chain->links, chain->len, unit->pub)
            != CARGO_RECEIPT_OK) {
            continue;
        }
        net_send_present_receipt_chain(unit->pub, chain);
    }
}

static void net_queue_pending_action_if_any(void) {
    uint8_t action = g.pending_net_action;
    if (action == NET_ACTION_NONE) return;

    uint8_t buy_grade = g.pending_net_buy_grade;
    int8_t place_station = g.pending_net_place_station;
    int8_t place_ring = g.pending_net_place_ring;
    int8_t place_slot = g.pending_net_place_slot;

    g.pending_net_action = NET_ACTION_NONE;
    g.pending_net_buy_grade = MINING_GRADE_COUNT;
    g.pending_net_place_station = -1;
    g.pending_net_place_ring = -1;
    g.pending_net_place_slot = -1;

    if (net_has_identity_pubkey()) {
        if (!net_has_identity_secret()) {
            fprintf(stderr,
                    "[net-action] blocked action=%u: identity-backed client missing signing secret\n",
                    (unsigned)action);
            set_notice("Signed action unavailable. Secret key missing.");
            return;
        }

        uint16_t action_id = net_next_action_id_alloc();
        uint8_t payload[7] = {
            action,
            buy_grade,
            (uint8_t)place_station,
            (uint8_t)place_ring,
            (uint8_t)place_slot,
            (uint8_t)(action_id & 0xFFu),
            (uint8_t)(action_id >> 8),
        };
        net_present_receipt_chains_for_action(action, buy_grade);
        if (net_send_signed_action(SIGNED_ACTION_INPUT_ACTION,
                                   payload, sizeof(payload))) {
            return;
        }

        fprintf(stderr,
                "[net-action] blocked action=%u: signed action path rejected for id=%u\n",
                (unsigned)action,
                (unsigned)action_id);
        set_notice("Unable to submit signed action. Action blocked for security.");
        return;
    }

    net_action_queue_push(action, buy_grade, place_station, place_ring,
                          place_slot);
}

static bool net_action_queue_peek_due(uint8_t *action, uint8_t *buy_grade,
                                      int8_t *place_station,
                                      int8_t *place_ring,
                                      int8_t *place_slot,
                                      uint16_t *action_id) {
    if (g.net_action_queue_count == 0) return false;
    net_action_queue_item_t *item = net_action_queue_at(0);
    if (!item->active || item->resend_timer > 0.0f) return false;
    *action = item->action;
    *buy_grade = item->buy_grade;
    *place_station = item->place_station;
    *place_ring = item->place_ring;
    *place_slot = item->place_slot;
    *action_id = item->action_id;
    return true;
}

static void net_action_queue_mark_sent(uint16_t input_seq) {
    if (g.net_action_queue_count == 0) return;
    net_action_queue_item_t *item = net_action_queue_at(0);
    if (item->first_input_seq == 0)
        item->first_input_seq = input_seq;
    else
        g.net_action_resend_packets++;
    item->send_count++;
    item->resend_timer = NET_ACTION_RESEND_SEC;
    g.net_action_packets_sent++;
}

static void net_track_input_send(uint16_t seq) {
    if (seq == 0) return;
    int index = (int)(seq % NET_INPUT_TIMING_CAP);
    g.net_input_timing[index].seq = seq;
    g.net_input_timing[index].sent_at = g.net_time;
}

static void frame(void) {
    float max_frame_dt = SIM_DT * (float)MAX_SIM_STEPS_PER_FRAME;
    float frame_dt = clampf((float)sapp_frame_duration(), 0.0f, max_frame_dt);
    g.net_time += frame_dt;

    /* --- Multiplayer: poll incoming and send input BEFORE sim --- */
    if (g.multiplayer_enabled) {
        bool was_connected = net_is_connected();
        net_poll();
        if (net_is_connected()) {
            g.net_ping_timer -= frame_dt;
            if (g.net_ping_timer <= 0.0f) {
                net_send_latency_ping();
                g.net_ping_timer = 1.0f;
            }
            g.net_metrics_timer -= frame_dt;
            if (g.net_metrics_timer <= 0.0f &&
                (g.net_ping_samples > 0 || g.net_motion.total_input_acks > 0)) {
                uint32_t seq = ++g.net_metrics_seq;
                if (seq == 0) seq = ++g.net_metrics_seq;
                net_send_client_metrics(
                    seq,
                    g.net_last_ping_rtt * 1000.0f,
                    g.net_last_ack_rtt * 1000.0f,
                    get_net_motion_last_ack_gap_ms(),
                    g.net_last_ping_server_turnaround_ms,
                    g.net_motion.packet_interval * 1000.0f,
                    (uint16_t)(g.net_input_seq - g.net_last_server_ack),
                    g.net_replay_count,
                    g.net_action_queue_count);
                g.net_metrics_timer = NET_CLIENT_METRICS_SEC;
            }
        }
        sync_local_player_slot_from_network();
        net_action_queue_update(frame_dt);
        net_queue_pending_action_if_any();
        if (was_connected && !net_is_connected()) {
            set_notice("Connection lost. Reload to reconnect.");
            /* world_t owns station/player manifest buffers; never copy it
             * by value. Use a fresh local sim as the crash-safe fallback. */
            world_cleanup(&g.local_server.world);
            local_server_init(&g.local_server, g.world.rng);
            local_server_sync_to_client(&g.local_server);
        }
        /* P key (offline): hard-reload the page. The HUD prompt is
         * "offline [P] reconnect" but a graceful net_reconnect()
         * never quite worked — sokol_app's browser context, sokol-gl
         * state, and the existing world snapshot all need a clean
         * boot to come back fully consistent. Just refresh. Native
         * builds keep the in-process reconnect path. */
        if (!net_is_connected() && g.local_server.active &&
            is_key_pressed(SAPP_KEYCODE_P)) {
#ifdef __EMSCRIPTEN__
            emscripten_run_script("window.location.reload()");
#else
            if (net_reconnect()) {
                set_notice("Reconnecting...");
                g.local_server.active = false;
                reset_remote_dynamic_sync();
            }
#endif
        }
        /* Send input immediately when controls change; otherwise keep a
         * low-rate heartbeat. The server persists the last input intent, so
         * unchanged movement does not need a fresh command every frame. */
        {
            uint8_t action = NET_ACTION_NONE;
            uint8_t buy_grade_byte = MINING_GRADE_COUNT;
            int8_t place_station = -1;
            int8_t place_ring = -1;
            int8_t place_slot = -1;
            uint16_t action_id = 0;
            bool action_due = net_action_queue_peek_due(
                &action, &buy_grade_byte, &place_station, &place_ring,
                &place_slot, &action_id);
            g.net_input_timer -= frame_dt;
            uint8_t flags = 0;
            input_intent_t movement_intent = {0};
            input_sample_movement(&movement_intent);
            if (movement_intent.thrust > 0.01f)
                flags |= NET_INPUT_THRUST;
            if (movement_intent.thrust < -0.01f)
                flags |= NET_INPUT_BRAKE;
            if (movement_intent.reverse_thrust)
                flags |= NET_INPUT_REVERSE;
            if (g.input.key_down[SAPP_KEYCODE_A] || g.input.key_down[SAPP_KEYCODE_LEFT])
                flags |= NET_INPUT_LEFT;
            if (g.input.key_down[SAPP_KEYCODE_D] || g.input.key_down[SAPP_KEYCODE_RIGHT])
                flags |= NET_INPUT_RIGHT;
            if (g.input.key_down[SAPP_KEYCODE_M])
                flags |= NET_INPUT_FIRE;
            if (g.input.key_down[SAPP_KEYCODE_SPACE] && !g.plan_mode_active)
                flags |= NET_INPUT_TRACTOR;
            if ((g.input.key_down[SAPP_KEYCODE_LEFT_SHIFT] ||
                 g.input.key_down[SAPP_KEYCODE_RIGHT_SHIFT]) &&
                !LOCAL_PLAYER.docked)
                flags |= NET_INPUT_BOOST;
            uint16_t mining_target =
                ((flags & NET_INPUT_FIRE) != 0 &&
                 LOCAL_PLAYER.hover_asteroid >= 0 &&
                 LOCAL_PLAYER.hover_asteroid < MAX_ASTEROIDS)
                ? (uint16_t)LOCAL_PLAYER.hover_asteroid : 0xFFFFu;
            bool input_changed = !g.net_input_have_last ||
                flags != g.net_last_sent_flags ||
                mining_target != g.net_last_sent_mining_target;
            bool active_controls = flags != 0;
            bool heartbeat_due = g.net_input_timer <= 0.0f;
            if (input_changed || heartbeat_due || action_due) {
                bool seq_advanced = false;
                if (input_changed || action != NET_ACTION_NONE) {
                    g.net_input_seq++;
                    if (g.net_input_seq == 0) g.net_input_seq++;
                    seq_advanced = true;
                }
                if (action != NET_ACTION_NONE &&
                    net_action_queue_head_first_send()) {
                    net_present_receipt_chains_for_action(action, buy_grade_byte);
                }
                uint32_t input_tick = net_next_input_apply_tick();
                net_send_input(flags, action, g.net_input_seq, mining_target,
                               buy_grade_byte, place_station, place_ring,
                               place_slot, action_id, input_tick);
                g.net_input_packets_sent++;
                if (seq_advanced) net_track_input_send(g.net_input_seq);
                if (action != NET_ACTION_NONE)
                    net_action_queue_mark_sent(g.net_input_seq);
                g.net_input_timer = active_controls
                    ? NET_ACTIVE_INPUT_HEARTBEAT_SEC
                    : NET_INPUT_HEARTBEAT_SEC;
                g.net_input_have_last = true;
                g.net_last_sent_flags = flags;
                g.net_last_sent_mining_target = mining_target;
            }
        }
    }

    advance_simulation_frame(frame_dt);

    /* Phase 2: keep the client-side manifest summary fresh. In SP this
     * reads the local manifest; in MP it's a no-op relative to the net
     * path which fills the summary directly (see TODO in client/net.c). */
    if (!g.multiplayer_enabled) refresh_station_manifest_summaries();


    audio_generate_stream(&g.audio);

    /* Upload the latest decoded episode frame once per render frame. Decoding
     * happens inside sim_step (possibly multiple steps per frame); uploading
     * here ensures at most one sg_update_image per image per frame. */
    episode_upload_frame(&g.episode);

    render_frame();
}

static void cleanup(void) {
    avatar_shutdown();
    episode_shutdown(&g.episode);
    music_shutdown(&g.music);
    if (g.multiplayer_enabled) {
        net_shutdown();
    }
    saudio_shutdown();
    sdtx_shutdown();
    hull_fog_shutdown();
    sgl_shutdown();
    sg_shutdown();
}

static void event(const sapp_event* event) {
    switch (event->type) {
        case SAPP_EVENTTYPE_KEY_DOWN: {
            /* Cast to int: sapp_keycode and KEY_COUNT are different enum
             * types and gcc -Werror=enum-compare rejects the direct
             * comparison. key_code values are always non-negative. */
            int kc = (int)event->key_code;
            if (kc >= 0 && kc < KEY_COUNT) {
                g.input.key_down[kc] = true;
                if (!event->key_repeat) {
                    g.input.key_pressed[kc] = true;
                }
            }
            if (event->key_code == SAPP_KEYCODE_ESCAPE) {
                sapp_request_quit();
            }
            break;
        }

        case SAPP_EVENTTYPE_KEY_UP: {
            int kc = (int)event->key_code;
            if (kc >= 0 && kc < KEY_COUNT) {
                g.input.key_down[kc] = false;
            }
            break;
        }

        case SAPP_EVENTTYPE_UNFOCUSED:
        case SAPP_EVENTTYPE_SUSPENDED:
        case SAPP_EVENTTYPE_ICONIFIED:
            clear_input_state();
            break;

        default:
            break;
    }
}

#ifdef __EMSCRIPTEN__
enum {
    MOBILE_CTRL_DOCKED            = 1u << 0,
    MOBILE_CTRL_IN_DOCK_RANGE     = 1u << 1,
    MOBILE_CTRL_DOCKING_APPROACH  = 1u << 2,
    MOBILE_CTRL_PLAN_ACTIVE       = 1u << 3,
    MOBILE_CTRL_PLAN_GHOST        = 1u << 4,
    MOBILE_CTRL_TOWING_SCAFFOLD   = 1u << 5,
    MOBILE_CTRL_TARGET_MODULE     = 1u << 6,
    MOBILE_CTRL_TARGET_DOCK       = 1u << 7,
    MOBILE_CTRL_INSPECTING_TARGET = 1u << 8,
    MOBILE_CTRL_STATION_DOCK      = 1u << 9,
    MOBILE_CTRL_STATION_TRADE     = 1u << 10,
    MOBILE_CTRL_STATION_WORK      = 1u << 11,
    MOBILE_CTRL_STATION_YARD      = 1u << 12,
    MOBILE_CTRL_HAS_CARGO         = 1u << 13,
    MOBILE_CTRL_AUTOPILOT_ON      = 1u << 14,
    MOBILE_CTRL_AUTOPILOT_READY   = 1u << 15,
    MOBILE_CTRL_CAN_FLIGHT        = 1u << 16,
    MOBILE_CTRL_CAN_MINE          = 1u << 17,
    MOBILE_CTRL_CAN_TRACTOR       = 1u << 18,
    MOBILE_CTRL_CAN_SCAN          = 1u << 19,
    MOBILE_CTRL_CAN_USE           = 1u << 20,
    MOBILE_CTRL_CAN_PLAN          = 1u << 21,
    MOBILE_CTRL_CAN_CYCLE         = 1u << 22,
    MOBILE_CTRL_CAN_PAGE          = 1u << 23,
    MOBILE_CTRL_CAN_SELL          = 1u << 24,
    MOBILE_CTRL_CAN_DIGITS        = 1u << 25,
    MOBILE_CTRL_CAN_REPAIR        = 1u << 26,
    MOBILE_CTRL_CAN_UPGRADE_MINE  = 1u << 27,
    MOBILE_CTRL_CAN_UPGRADE_HOLD  = 1u << 28,
    MOBILE_CTRL_CAN_UPGRADE_TOW   = 1u << 29,
};

EMSCRIPTEN_KEEPALIVE
int signal_mobile_control_flags(void) {
    uint32_t flags = MOBILE_CTRL_CAN_SCAN;
    const bool docked = LOCAL_PLAYER.docked;

    if (docked) {
        const station_t *st = current_station_ptr();
        if (!station_panel_visible(station_panel_descriptor(g.station_view), st))
            g.station_view = station_panel_first_visible(st);

        flags |= MOBILE_CTRL_DOCKED | MOBILE_CTRL_CAN_USE;
        switch (g.station_view) {
        case STATION_VIEW_DOCK:  flags |= MOBILE_CTRL_STATION_DOCK; break;
        case STATION_VIEW_TRADE: flags |= MOBILE_CTRL_STATION_TRADE |
                                       MOBILE_CTRL_CAN_PAGE |
                                       MOBILE_CTRL_CAN_SELL |
                                       MOBILE_CTRL_CAN_DIGITS; break;
        case STATION_VIEW_WORK:  flags |= MOBILE_CTRL_STATION_WORK |
                                       MOBILE_CTRL_CAN_SELL |
                                       MOBILE_CTRL_CAN_DIGITS; break;
        case STATION_VIEW_YARD:  flags |= MOBILE_CTRL_STATION_YARD |
                                       MOBILE_CTRL_CAN_DIGITS; break;
        case STATION_VIEW_COUNT: break;
        }

        if (g.station_view == STATION_VIEW_DOCK) {
            station_ui_state_t ui = { 0 };
            build_station_ui_state(&ui);
            if (ui.can_repair)          flags |= MOBILE_CTRL_CAN_REPAIR;
            if (ui.can_upgrade_mining)  flags |= MOBILE_CTRL_CAN_UPGRADE_MINE;
            if (ui.can_upgrade_hold)    flags |= MOBILE_CTRL_CAN_UPGRADE_HOLD;
            if (ui.can_upgrade_tractor) flags |= MOBILE_CTRL_CAN_UPGRADE_TOW;
        }
    } else {
        flags |= MOBILE_CTRL_CAN_FLIGHT;
        if (!g.plan_mode_active) {
            flags |= MOBILE_CTRL_CAN_MINE | MOBILE_CTRL_CAN_TRACTOR;
        }
        if (!g.plan_mode_active && LOCAL_PLAYER.ship.towed_scaffold < 0)
            flags |= MOBILE_CTRL_CAN_PLAN;
    }

    if (LOCAL_PLAYER.in_dock_range) flags |= MOBILE_CTRL_IN_DOCK_RANGE;
    if (LOCAL_PLAYER.docking_approach) flags |= MOBILE_CTRL_DOCKING_APPROACH;
    if (ship_total_cargo(&LOCAL_PLAYER.ship) > 0.0f)
        flags |= MOBILE_CTRL_HAS_CARGO;

    if (g.plan_mode_active) {
        flags |= MOBILE_CTRL_PLAN_ACTIVE | MOBILE_CTRL_CAN_USE |
                 MOBILE_CTRL_CAN_PLAN | MOBILE_CTRL_CAN_CYCLE;
        if (g.plan_target_station < 0) flags |= MOBILE_CTRL_PLAN_GHOST;
    }

    if (!docked && LOCAL_PLAYER.ship.towed_scaffold >= 0)
        flags |= MOBILE_CTRL_TOWING_SCAFFOLD | MOBILE_CTRL_CAN_USE;

    if (!docked && LOCAL_PLAYER.in_dock_range)
        flags |= MOBILE_CTRL_CAN_USE;

    if (!docked && g.target_station >= 0 && g.target_station < MAX_STATIONS &&
        g.target_module >= 0) {
        const station_t *st = &g.world.stations[g.target_station];
        if (g.target_module < st->module_count) {
            flags |= MOBILE_CTRL_TARGET_MODULE | MOBILE_CTRL_CAN_USE;
            if (st->modules[g.target_module].type == MODULE_DOCK)
                flags |= MOBILE_CTRL_TARGET_DOCK;
            if (g.inspect_station == g.target_station &&
                g.inspect_module == g.target_module)
                flags |= MOBILE_CTRL_INSPECTING_TARGET;
        }
    }

    if (LOCAL_PLAYER.autopilot_mode) {
        flags |= MOBILE_CTRL_AUTOPILOT_ON | MOBILE_CTRL_AUTOPILOT_READY;
    } else if (signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos) >=
               SIGNAL_BAND_OPERATIONAL) {
        flags |= MOBILE_CTRL_AUTOPILOT_READY;
    }

    return (int)flags;
}

EMSCRIPTEN_KEEPALIVE
int signal_mobile_digit_mask(void) {
    if (!LOCAL_PLAYER.docked) return 0;
    const station_t *st = current_station_ptr();
    if (!st) return 0;
    if (!station_panel_visible(station_panel_descriptor(g.station_view), st))
        g.station_view = station_panel_first_visible(st);

    int mask = 0;
    switch (g.station_view) {
    case STATION_VIEW_TRADE: {
        trade_row_t rows[TRADE_MAX_ROWS];
        int row_count = build_trade_rows(st, &LOCAL_PLAYER.ship, rows, TRADE_MAX_ROWS);
        int page_first = 0, page_last = 0, total_pages = 1;
        trade_page_range(rows, row_count, (int)g.trade_page,
                         &page_first, &page_last, &total_pages);
        if ((int)g.trade_page >= total_pages) {
            trade_page_range(rows, row_count, 0,
                             &page_first, &page_last, &total_pages);
        }
        for (int i = 0; i < 5 && page_first + i < page_last; i++) {
            if (rows[page_first + i].actionable) mask |= (1 << i);
        }
        break;
    }
    case STATION_VIEW_WORK: {
        int slot_contract[3] = {-1, -1, -1};
        bool slot_fulfillable[3] = {false, false, false};
        int slot_held[3] = {0, 0, 0};
        vec2 here_pos = st->pos;
        (void)build_work_slots(LOCAL_PLAYER.current_station, here_pos,
                               slot_contract, slot_fulfillable, slot_held);
        for (int i = 0; i < 3; i++) {
            if (slot_contract[i] >= 0) mask |= (1 << i);
        }
        break;
    }
    case STATION_VIEW_YARD: {
        if (!station_has_module(st, MODULE_SHIPYARD)) break;
        int shown = 0;
        for (int t = 0; t < MODULE_COUNT && shown < 5; t++) {
            module_type_t kit = (module_type_t)t;
            if (module_kind(kit) == MODULE_KIND_NONE) continue;
            if (!station_has_module(st, kit)) continue;
            if (!module_unlocked_for_player(LOCAL_PLAYER.ship.unlocked_modules, kit))
                continue;
            mask |= (1 << shown);
            shown++;
        }
        break;
    }
    case STATION_VIEW_DOCK:
    case STATION_VIEW_COUNT:
        break;
    }

    return mask;
}

static sapp_keycode mobile_action_key(int action) {
    switch (action) {
    case 1:  return SAPP_KEYCODE_W;          /* thrust */
    case 2:  return SAPP_KEYCODE_S;          /* brake / sell */
    case 3:  return SAPP_KEYCODE_A;          /* turn left */
    case 4:  return SAPP_KEYCODE_D;          /* turn right */
    case 5:  return SAPP_KEYCODE_M;          /* mine / upgrade mining */
    case 6:  return SAPP_KEYCODE_SPACE;      /* tractor / tow release */
    case 7:  return SAPP_KEYCODE_E;          /* interact / launch / place */
    case 8:  return SAPP_KEYCODE_H;          /* hail / scan */
    case 9:  return SAPP_KEYCODE_LEFT_SHIFT; /* boost */
    case 10: return SAPP_KEYCODE_B;          /* plan mode */
    case 11: return SAPP_KEYCODE_R;          /* repair / cycle plan */
    case 12: return SAPP_KEYCODE_TAB;        /* station view */
    case 13: return SAPP_KEYCODE_F;          /* trade page */
    case 14: return SAPP_KEYCODE_S;          /* sell / deliver */
    case 15: return SAPP_KEYCODE_C;          /* cargo hold upgrade */
    case 16: return SAPP_KEYCODE_T;          /* tractor upgrade */
    case 20: return SAPP_KEYCODE_1;
    case 21: return SAPP_KEYCODE_2;
    case 22: return SAPP_KEYCODE_3;
    case 23: return SAPP_KEYCODE_4;
    case 24: return SAPP_KEYCODE_5;
    case 30: return SAPP_KEYCODE_O;          /* autopilot */
    case 31: return SAPP_KEYCODE_ESCAPE;     /* back / close */
    default: return SAPP_KEYCODE_INVALID;
    }
}

EMSCRIPTEN_KEEPALIVE
void signal_mobile_key(int action, int down) {
    int kc = (int)mobile_action_key(action);
    if (kc < 0 || kc >= KEY_COUNT) return;

    if (down) {
        if (!g.input.key_down[kc])
            g.input.key_pressed[kc] = true;
        g.input.key_down[kc] = true;
    } else {
        g.input.key_down[kc] = false;
    }
}

EMSCRIPTEN_KEEPALIVE
void signal_mobile_clear(void) {
    clear_input_state();
}
#endif

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 1600,
        .height = 900,
        .sample_count = 4,
        .high_dpi = true,
        .window_title = "SIGNAL",
        .logger.func = slog_func,
    };
}
