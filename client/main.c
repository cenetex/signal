#include <stdarg.h>
#include <stdlib.h>

#include "client.h"
#include "audio.h"
#include "camera_model.h"
#include "npc.h"
#include "render.h"
#include "rng.h"
#include "net.h"
#include "world_draw.h"
#include "signal_model.h"
#include "input.h"
#include "net_sync.h"
#include "onboarding.h"
#include "story_runtime.h"
#include "progress_store.h"
#include "avatar.h"
#include "mining_client.h"
#include "neural_singleplayer.h"
#include "base58.h"
#include "manifest.h"
#include "contract_objective.h"
#include "palette.h"
#include "signal_intelligence.h"
#include "gossip.h"
#include "net_input_lead.h"
#include "net_clock.h"
#include "client_log.h"
#include "hud_attention.h"
#include "legacy_recovery_ui.h"
#include "gameplay_observability.h"
#include "asteroid_presentation.h"


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

#ifdef __EMSCRIPTEN__
static bool g_mobile_virtual_key_down[KEY_COUNT];

static void mobile_restore_virtual_keys(void) {
    for (int kc = 0; kc < KEY_COUNT; kc++) {
        if (g_mobile_virtual_key_down[kc]) {
            g.input.key_down[kc] = true;
        }
    }
}

static void mobile_clear_virtual_keys(void) {
    memset(g_mobile_virtual_key_down, 0, sizeof(g_mobile_virtual_key_down));
}

static void clear_input_state_for_canvas_focus_loss(void) {
    clear_input_state();
    mobile_restore_virtual_keys();
}
#endif

static const int MAX_SIM_STEPS_PER_FRAME = 8;

static float camera_view_narrow_focus(float fallback_w, float fallback_h) {
#ifdef __EMSCRIPTEN__
    int css_w = emscripten_run_script_int(
        "(function(){var c=(window.SignalGameModule&&window.SignalGameModule.canvas)"
        "||document.getElementById('canvas');"
        "return c?Math.round(c.clientWidth):Math.round(window.innerWidth||0);})()");
    int css_h = emscripten_run_script_int(
        "(function(){var c=(window.SignalGameModule&&window.SignalGameModule.canvas)"
        "||document.getElementById('canvas');"
        "return c?Math.round(c.clientHeight):Math.round(window.innerHeight||0);})()");
    if (css_w > 0 && css_h > 0)
        return camera_narrow_focus((float)css_w, (float)css_h);
#endif
    return camera_narrow_focus(fallback_w, fallback_h);
}

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

#define NET_INPUT_HEARTBEAT_SEC ((float)NET_INPUT_IDLE_HEARTBEAT_MS / 1000.0f)
#define NET_ACTIVE_INPUT_HEARTBEAT_SEC ((float)NET_INPUT_ACTIVE_HEARTBEAT_MS / 1000.0f)
#define NET_ACTIVE_INPUT_ACK_HEARTBEAT_SEC ((float)NET_INPUT_ACTIVE_ACK_HEARTBEAT_MS / 1000.0f)
#define NET_ACTIVE_INPUT_ACK_RECOVERY_GAP_SEC 0.250f
#define NET_ACTIVE_INPUT_ACK_RECOVERY_UNACKED 4u
#define NET_ACTIVE_INPUT_ACK_HOT_RECOVERY_GAP_SEC 0.500f
#define NET_ACTIVE_INPUT_ACK_HOT_RECOVERY_UNACKED 8u
#define NET_ACTIVE_INPUT_ACK_AGE_RECOVERY_MIN_SEC 0.250f
#define NET_ACTIVE_INPUT_ACK_AGE_RECOVERY_RTT_MULT 2.5f
#define NET_ACTION_RESEND_SEC (1.0f / 12.0f)
#define NET_ACTION_RETRY_SEC 6.0f
#define NET_CLIENT_METRICS_SEC 15.0f
#define NET_PING_BOOT_INTERVAL_SEC 1.0f
#define NET_PING_RECOVERY_INTERVAL_SEC 0.5f
#define NET_PING_STEADY_INTERVAL_SEC 2.0f
#define NET_PING_QUIET_INTERVAL_SEC 2.5f
#define NET_PING_ACK_SAMPLED_QUIET_INTERVAL_SEC 5.0f
#define NET_PING_ACK_GAP_RECOVERY_SEC 0.250f
#define NET_PING_MAX_WINDOW_SEC 5.0f
#define NET_CONTROL_LANE_STABLE_GAP_SEC 0.125f
#define NET_CONTROL_LANE_QUIET_MAX_UNACKED 1u
#define LOCAL_PLAYER_RENDER_CORRECTION_SEC 0.06f
#define LOCAL_PLAYER_RENDER_CORRECTION_LATENCY_SEC 0.10f

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
static void on_remote_input_applied(uint16_t input_seq, uint32_t server_tick,
                                    uint32_t input_tick_ack,
                                    uint32_t client_sent_ms,
                                    uint32_t server_recv_ms,
                                    uint32_t server_send_ms);
static void on_remote_handoff_ticket(uint8_t status, uint8_t source_station,
                                     uint8_t dest_station,
                                     const handoff_ticket_t *ticket);
static void on_remote_handoff_result(uint8_t status, uint8_t reason,
                                     uint8_t dest_station,
                                     const uint8_t ticket_hash[32]);
static void on_remote_latency_sample(uint32_t seq, float rtt_ms,
                                     float server_turnaround_ms,
                                     uint32_t server_tick);
static void on_remote_protocol_info(const NetProtocolInfo *info);
static void on_remote_legacy_recovery_offer(
    const uint8_t offer_id[LEGACY_RECOVERY_OFFER_ID_SIZE],
    uint16_t expires_in_seconds);
static void on_remote_legacy_recovery_result(
    legacy_recovery_result_status_t status);

static legacy_recovery_ui_t legacy_recovery_ui;
static char legacy_recovery_disconnect_notice[192];
#ifdef __EMSCRIPTEN__
static bool legacy_recovery_smoke_active;
static bool legacy_recovery_smoke_send_admitted = true;
static uint32_t legacy_recovery_smoke_confirm_count;
static uint32_t legacy_recovery_smoke_cancel_count;
static uint32_t legacy_recovery_smoke_expire_count;
#endif

static void configure_net_callbacks(NetCallbacks *cbs) {
    if (!cbs) return;
    memset(cbs, 0, sizeof(*cbs));
    cbs->on_join = on_player_join;
    cbs->on_leave = on_player_leave;
    cbs->on_players_begin = begin_player_state_batch;
    cbs->on_state = apply_remote_player_state;
    cbs->on_input_applied = on_remote_input_applied;
    cbs->on_asteroids = apply_remote_asteroids;
    cbs->on_asteroid_motion = apply_remote_asteroid_motion;
    cbs->on_asteroid_state_q = apply_remote_asteroid_state_q;
    cbs->on_npcs = apply_remote_npcs;
    cbs->on_npc_motion = apply_remote_npc_motion;
    cbs->on_npc_pos = apply_remote_npc_pos;
    cbs->on_npc_pose = apply_remote_npc_pose;
    cbs->on_npc_linear = apply_remote_npc_linear;
    cbs->on_npc_status = apply_remote_npc_status;
    cbs->on_stations = apply_remote_stations;
    cbs->on_station_identity = apply_remote_station_identity;
    cbs->on_station_diag = apply_remote_station_diag;
    cbs->on_scaffolds = apply_remote_scaffolds;
    cbs->on_scaffold_remove = apply_remote_scaffold_remove;
    cbs->on_scaffold_motion = apply_remote_scaffold_motion;
    cbs->on_cargo_pods = apply_remote_cargo_pods;
    cbs->on_cargo_pod_remove = apply_remote_cargo_pod_remove;
    cbs->on_cargo_pod_motion = apply_remote_cargo_pod_motion;
    cbs->on_cargo_pod_linear = apply_remote_cargo_pod_linear;
    cbs->on_interactions = apply_remote_interactions;
    cbs->on_interaction_drift = apply_remote_interaction_drift;
    cbs->on_tow_links = apply_remote_tow_links;
    cbs->on_hail_response = apply_remote_hail_response;
    cbs->on_player_ship = apply_remote_player_ship;
    cbs->on_contracts = apply_remote_contracts;
    cbs->on_player_known_contracts = apply_remote_player_known_contracts;
    cbs->on_player_market_memories = apply_remote_player_market_memories;
    cbs->on_player_known_ledger = apply_remote_player_known_ledger;
    cbs->on_delivery_ledger = apply_remote_delivery_ledger;
    cbs->on_death = on_remote_death;
    cbs->on_world_time = on_remote_world_time;
    cbs->on_events = apply_remote_events;
    cbs->on_signal_channel = apply_remote_signal_channel;
    cbs->on_station_manifest = apply_remote_station_manifest;
    cbs->on_player_manifest = apply_remote_player_manifest;
    cbs->on_cargo_receipt_bundle = apply_remote_cargo_receipt_bundle;
    cbs->on_inspect_snapshot = apply_remote_inspect_snapshot;
    cbs->on_highscores = apply_remote_highscores;
    cbs->on_action_ack = on_remote_action_ack;
    cbs->on_action_result = on_remote_action_result;
    cbs->on_latency_sample = on_remote_latency_sample;
    cbs->on_protocol_info = on_remote_protocol_info;
    cbs->on_handoff_ticket = on_remote_handoff_ticket;
    cbs->on_handoff_result = on_remote_handoff_result;
    cbs->on_legacy_recovery_offer =
        on_remote_legacy_recovery_offer;
    cbs->on_legacy_recovery_result =
        on_remote_legacy_recovery_result;
}

static void on_remote_protocol_info(const NetProtocolInfo *info) {
    if (!info) return;
    g.net_server_protocol_version = info->version;
    if (!info->compatible ||
        info->version != SIGNAL_PROTOCOL_VERSION) {
        g.net_protocol_incompatible = true;
    }
}

static void on_remote_legacy_recovery_offer(
    const uint8_t offer_id[LEGACY_RECOVERY_OFFER_ID_SIZE],
    uint16_t expires_in_seconds) {
    /*
     * net.c retains the exact opaque offer. Presentation state receives only
     * its bounded lifetime, so no offer/token/path bytes can reach UI text or
     * browser exports.
     */
    (void)offer_id;
    if (!offer_id || expires_in_seconds == 0) return;
    if (!legacy_recovery_ui_begin_offer(
            &legacy_recovery_ui, net_now_ms32(),
            expires_in_seconds)) {
        return;
    }
    g.input.key_pressed[SAPP_KEYCODE_ENTER] = false;
    g.input.key_pressed[SAPP_KEYCODE_KP_ENTER] = false;
    g.input.key_pressed[SAPP_KEYCODE_ESCAPE] = false;
    snprintf(
        legacy_recovery_disconnect_notice,
        sizeof(legacy_recovery_disconnect_notice),
        "%s",
        "Legacy recovery was not completed; the remote save is untouched.");
    set_notice(
        "Legacy save available for this verified session. "
        "[ENTER] recover, [ESC] leave untouched (%us).",
        (unsigned)expires_in_seconds);
}

static const char *legacy_recovery_result_notice(
    legacy_recovery_result_status_t status) {
    switch (status) {
    case LEGACY_RECOVERY_RESULT_NO_MATCH:
        return "No matching legacy save remained; nothing was changed.";
    case LEGACY_RECOVERY_RESULT_STALE_OFFER:
        return "Legacy recovery offer expired; reconnect to retry.";
    case LEGACY_RECOVERY_RESULT_REPLAY:
        return "Legacy recovery confirmation was already used.";
    case LEGACY_RECOVERY_RESULT_INVALID_SOURCE:
        return "Legacy save is invalid or corrupt; nothing was changed.";
    case LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT:
        return "An existing identity save won; it was not overwritten.";
    case LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE:
        return "Legacy recovery could not commit; nothing was changed.";
    case LEGACY_RECOVERY_RESULT_SUCCESS:
        return "Legacy save recovered. Authoritative state refreshed.";
    default:
        return "Legacy recovery returned an unknown result.";
    }
}

static void on_remote_legacy_recovery_result(
    legacy_recovery_result_status_t status) {
    if (!legacy_recovery_ui_apply_result(
            &legacy_recovery_ui, status, net_now_ms32())) {
        return;
    }
    const char *notice = legacy_recovery_result_notice(status);
    if (status == LEGACY_RECOVERY_RESULT_SUCCESS) {
        memset(legacy_recovery_disconnect_notice, 0,
               sizeof(legacy_recovery_disconnect_notice));
    } else {
        snprintf(
            legacy_recovery_disconnect_notice,
            sizeof(legacy_recovery_disconnect_notice),
            "%s", notice);
    }
    set_notice("%s", notice);
}

static void handle_net_protocol_mismatch(void) {
    if (!g.net_protocol_incompatible ||
        g.net_protocol_mismatch_handled) {
        return;
    }
    g.net_protocol_mismatch_handled = true;
    net_shutdown();
    set_notice(
        "Protocol mismatch (client v%u, server v%u). Update/reload required.",
        (unsigned)SIGNAL_PROTOCOL_VERSION,
        (unsigned)g.net_server_protocol_version);
#ifdef __EMSCRIPTEN__
    int already_reloaded = emscripten_run_script_int(
        "new URLSearchParams(location.search).has('pv') ? 1 : 0");
    if (!already_reloaded) {
        char reload_script[512];
        snprintf(
            reload_script, sizeof(reload_script),
            "(() => {"
            "  const u = new URL(location.href);"
            "  u.searchParams.set('pv', '%u');"
            "  u.searchParams.set('v', Date.now().toString());"
            "  location.replace(u.pathname + '?' + "
            "u.searchParams.toString() + u.hash);"
            "})()",
            (unsigned)SIGNAL_PROTOCOL_VERSION);
        emscripten_run_script(reload_script);
    }
#endif
}

/* Keep singleplayer on the dedicated server's bounded cadences by default.
 * Per-tick snapshots are still available as an explicit diagnostic mode.
 * See local_server_t.throttled_snapshots. */
static bool local_throttled_snapshots_requested(void) {
#ifdef __EMSCRIPTEN__
    const char *v = emscripten_run_script_string(
        "(new URLSearchParams(window.location.search).get('netcadence')"
        " ?? '1')");
    return !v || v[0] != '0';
#else
    const char *v = getenv("SIGNAL_LOCAL_NET_CADENCE");
    return !(v && v[0] == '0');
#endif
}

static bool start_local_loopback_authority(const NetCallbacks *cbs) {
    if (!local_server_has_world(&g.local_server) ||
        !g.local_server.active) {
        return false;
    }
    /* Set before the initial snapshot: the advertised protocol cadences
     * depend on the mode. Every loopback session passes through here, so
     * the flag survives local_server_init() on any reset path. */
    g.local_server.throttled_snapshots = local_throttled_snapshots_requested();
    local_server_attach_loopback(&g.local_server);
    if (!net_init_loopback(cbs, 0))
        return false;
    reset_remote_dynamic_sync();
    if (!local_server_send_initial_snapshot(
            &g.local_server, g.local_player_slot)) {
        net_shutdown();
        return false;
    }
    sync_local_player_slot_from_network();
    return true;
}

static bool start_fresh_local_authority(const NetCallbacks *cbs) {
    net_shutdown();
    g.net_authority_enabled = false;
    g.local_player_slot = 0;
    local_server_shutdown(&g.local_server);
    if (!local_server_init(&g.local_server, 0)) return false;
    neural_singleplayer_init();
    if (start_local_loopback_authority(cbs)) {
        g.net_authority_enabled = true;
        return true;
    }
    local_server_shutdown(&g.local_server);
    return false;
}

static bool net_tick_after_u32(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static bool net_input_seq_after_u16(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

static uint16_t net_unacked_input_count(void) {
    if (g.net_input_seq == 0) return 0;
    if (g.net_last_server_ack == 0) return g.net_input_seq;
    if (g.net_input_seq == g.net_last_server_ack) return 0;
    if (!net_input_seq_after_u16(g.net_input_seq, g.net_last_server_ack))
        return 0;
    return (uint16_t)(g.net_input_seq - g.net_last_server_ack);
}

static bool net_input_timing_seq_unacked(uint16_t seq) {
    if (seq == 0 || g.net_input_seq == 0) return false;
    if (seq == g.net_last_server_ack) return false;
    if (g.net_last_server_ack != 0 &&
        !net_input_seq_after_u16(seq, g.net_last_server_ack)) {
        return false;
    }
    if (net_input_seq_after_u16(seq, g.net_input_seq))
        return false;
    return true;
}

static float net_input_timing_age_sec(const net_input_timing_t *timing) {
    if (!timing || timing->seq == 0) return 0.0f;
    if (timing->sent_ms != 0) {
        uint32_t elapsed_ms = net_now_ms32() - timing->sent_ms;
        return (float)elapsed_ms / 1000.0f;
    }
    return g.net_time - timing->sent_at;
}

static float net_oldest_unacked_input_age_sec(void) {
    float oldest = 0.0f;
    for (int i = 0; i < NET_INPUT_TIMING_CAP; i++) {
        const net_input_timing_t *timing = &g.net_input_timing[i];
        if (!net_input_timing_seq_unacked(timing->seq))
            continue;
        float age = net_input_timing_age_sec(timing);
        if (isfinite(age) && age > oldest)
            oldest = age;
    }
    return oldest;
}

static uint32_t net_input_lead_ticks(void) {
    if (net_is_loopback()) return NET_INPUT_LEAD_MIN_TICKS;
    float rtt = net_prediction_control_rtt_sec();
    return net_input_lead_ticks_from_rtt(
        rtt, SIM_DT, g.net_motion.input_lead_margin_ticks);
}

static uint32_t net_next_input_apply_tick(void) {
    if (g.net_last_server_tick != 0) {
        uint32_t server_tick =
            net_estimated_server_tick_now(g.net_last_server_tick);
        uint32_t target = server_tick + net_input_lead_ticks();
        if (!net_tick_after_u32(target, server_tick))
            target = server_tick + 1u;
        return target;
    }
    if (g.net_prediction_tick_valid) return g.net_prediction_tick + 1u;
    return 0u;
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
    g.camera_initialized = false;
    g.boost_zoom = 1.0f;
    g.cargo_focus_zoom = 1.0f;
    g.boost_center_blend = 0.0f;

    /* Server-managed world: the client predicts until authoritative snapshots
     * arrive over either remote WebSocket or local loopback.  world_reset()
     * already releases every owned subobject and clears the full world while
     * preserving its reusable signal-cache allocation; an outer cleanup plus
     * memset duplicated a 16 MiB sweep and defeated that reuse. */
    world_reset(&g.world);
    player_init_ship(&LOCAL_PLAYER, &g.world);
    LOCAL_PLAYER.connected = true;

    g.tracked_contract = -1;
    g.selected_contract = -1;
    g.trade_lineage_row = -1;
    g.trade_lineage_proof = false;
    g.trade_lineage_proof_page = 0;
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
    g.inspect_receipt_page = 0;
    g.inspect_receipt_browser = false;
    g.inspect_was_active = false;
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    reset_local_asteroid_motion_telemetry();
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    memset(&g.scaffold_interp, 0, sizeof(g.scaffold_interp));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));
    memset(&g.player_interp, 0, sizeof(g.player_interp));
    g.player_interp.interval = 0.1f;
    memset(g.scanned_players, 0, sizeof(g.scanned_players));
    reset_station_ring_smoothing();

    /* Seed interp buffers so first frame has valid data */
    memcpy(g.asteroid_interp.curr, g.world.asteroids, sizeof(g.asteroid_interp.curr));
    memcpy(g.asteroid_interp.prev, g.world.asteroids, sizeof(g.asteroid_interp.prev));
    memcpy(g.scaffold_interp.curr, g.world.scaffolds, sizeof(g.scaffold_interp.curr));
    memcpy(g.scaffold_interp.prev, g.world.scaffolds, sizeof(g.scaffold_interp.prev));
    memcpy(g.cargo_pod_interp.curr, g.world.cargo_pods, sizeof(g.cargo_pod_interp.curr));
    memcpy(g.cargo_pod_interp.prev, g.world.cargo_pods, sizeof(g.cargo_pod_interp.prev));

    g.thrusting = false;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    g.net_time = 0.0f;
    net_reset_local_input_stream();
    g.net_last_ack_rtt = 0.0f;
    g.net_last_ping_raw_rtt = 0.0f;
    g.net_last_ping_rtt = 0.0f;
    g.net_last_ping_server_turnaround_ms = 0.0f;
    g.net_last_dedicated_ping_sample_time = 0.0f;
    g.net_last_ack_transport_sample_time = 0.0f;
    net_latency_stats_reset(&g.net_ack_latency);
    net_latency_stats_reset(&g.net_ping_latency);
    net_latency_gap_stats_reset(&g.net_ack_gap);
    g.net_max_ping_rtt_5s = 0.0f;
    g.net_ping_samples = 0;
    g.net_ping_timer = 0.0f;
    g.net_metrics_timer = 0.0f;
    g.net_metrics_seq = 0;
    g.net_missed_pongs = 0;
    g.net_missed_input_acks = 0;
    g.net_ack_recovery_packets = 0;
    g.net_ping_miss_windows_reported = 0;
    g.net_ack_miss_windows_reported = 0;
    g.net_ack_recovery_tier = NET_LATENCY_ACK_RECOVERY_STEADY;
    g.net_max_ack_rtt_5s = 0.0f;
    g.net_ack_window_elapsed = 0.0f;
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
    /* Beam prediction under network authority: predict beam START position
     * from local ship state (eliminates snapshot-rate lag on muzzle position).
     * Server owns beam_active, beam_hit, beam_ineffective, and beam_end. */
    if (g.net_authority_enabled) {
        /* Only predict the muzzle — server owns everything else */
        if (LOCAL_PLAYER.beam_active) {
            LOCAL_PLAYER.beam_start = ship_muzzle(LOCAL_PLAYER.ship->pos,
                LOCAL_PLAYER.ship->angle, LOCAL_PLAYER.ship);
        }
    } else {
        LOCAL_PLAYER.beam_active = false;
        LOCAL_PLAYER.beam_hit = false;
    }
    g.thrusting = false;
}

/* sample_input_intent: see input.h/c */

/* Rebuild g.station_manifest_summary from local station manifests.
 * Offline fallback: rebuild g.station_manifest_summary from local station
 * manifests. Network-authoritative sessions receive
 * NET_MSG_STATION_MANIFEST and populate the summary in net_sync.c. */
static void refresh_station_manifest_summaries(void) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        /* Zero the row — a station with no manifest units should read zero. */
        memset(&g.station_manifest_summary[s][0][0], 0,
               sizeof(g.station_manifest_summary[s]));
        const station_t *st = &g.world.stations[s];
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            g.station_stock_summary[s][c] =
                station_inventory_amount(st, (commodity_t)c);
        }
        g.station_stock_summary_valid[s] = station_exists(st);
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

static bool local_station_balance_for_player(int station_idx, float *out);
static void maybe_notice_local_credits_rule(int station_idx, float balance);

static void defer_episode_until_docked(int episode_index) {
    if (episode_index < 0 || episode_index >= EPISODE_COUNT) return;
    g.deferred_episode_mask |= (uint16_t)(1u << episode_index);
    g.deferred_episode_timer = 2.5f;
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
    onboarding_mark_docked_after_earning();
    int ds = LOCAL_PLAYER.current_station;
    float bal = 0.0f;
    if (local_station_balance_for_player(ds, &bal))
        maybe_notice_local_credits_rule(ds, bal);
    if (ds < SIGNAL_ROOT_STATION_COUNT) {
        g.episode.stations_visited |= (1 << ds);
        if (g.episode.stations_visited == ((1u << SIGNAL_ROOT_STATION_COUNT) - 1u)) /* all relay roots */
            episode_trigger(&g.episode, 1); /* Ep 1: Kepler's Law */
    }
    char story_notice[192];
    if (story_runtime_mark_dock(ds, story_notice, sizeof(story_notice)))
        set_notice("%s", story_notice);
}

static void sim_on_launch(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_launch(&g.audio);
    g.screen_shake = fmaxf(g.screen_shake, 5.0f); /* launch kick */
    if (g.onboarding.complete)
        episode_trigger(&g.episode, 0); /* returning pilot: launch is quiet enough */
    else
        defer_episode_until_docked(0); /* newcomer: never cover flight teaching */
    if (!g.music.playing && !g.music.loading) music_next_track(&g.music);
}

/* Roll the per-frame sale-fx + hint-bar batch state for one SELL event. */
static vec2 sell_event_world_pos(const sim_event_t *ev) {
    const station_t *st = &g.world.stations[ev->sell.station];
    int module_idx = -1;
    if (ev->sell.quantity > 0 &&
        ev->sell.module < st->module_count) {
        module_idx = (int)ev->sell.module;
    } else if (ev->sell.quantity > 0 &&
               ev->sell.commodity < COMMODITY_COUNT) {
        module_idx = station_find_hopper_for(
            st, (commodity_t)ev->sell.commodity);
    }
    if (module_idx >= 0 && module_idx < st->module_count) {
        const station_module_t *m = &st->modules[module_idx];
        return module_world_pos_ring(st, m->ring, m->slot);
    }
    return st->pos;
}

static void sell_batch_accumulate(const sim_event_t *ev, int total) {
    if (ev->sell.station < 0 || ev->sell.station >= MAX_STATIONS) return;
    if (!station_exists(&g.world.stations[ev->sell.station])) return;
    vec2 payout_pos = sell_event_world_pos(ev);
    spawn_sell_fx(&payout_pos, total,
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
    mining_client_record_strike((mining_grade_t)ev->sell.grade, ev->sell.bonus_cr);
    int total = ev->sell.base_cr + ev->sell.bonus_cr;
    if (total > 0) {
        onboarding_mark_earned();
        /* The furnace milestone supersedes the launch intro and waits until
         * the player has a quiet docked moment to read it. */
        g.deferred_episode_mask &=
            (uint16_t)(UINT16_MAX ^ (uint16_t)(1u << 0));
        defer_episode_until_docked(2);
        sell_batch_accumulate(ev, total);

        if (ev->sell.quantity > 0 &&
            ev->sell.commodity < COMMODITY_COUNT &&
            ev->sell.station >= 0 &&
            ev->sell.station < MAX_STATIONS) {
            const station_t *dest = &g.world.stations[ev->sell.station];
            const char *currency = dest->currency_name[0]
                ? dest->currency_name : "credits";
            bool known_origin =
                ev->sell.origin_station < MAX_STATIONS &&
                station_exists(&g.world.stations[ev->sell.origin_station]);
            if (known_origin &&
                ev->sell.origin_station == (uint8_t)ev->sell.station) {
                set_notice("%s smelted %u %s // +%d %s",
                           dest->name, (unsigned)ev->sell.quantity,
                           commodity_name((commodity_t)ev->sell.commodity),
                           total, currency);
            } else if (known_origin) {
                set_notice("%s intake accepted %u %s from %s // +%d %s",
                           dest->name, (unsigned)ev->sell.quantity,
                           commodity_name((commodity_t)ev->sell.commodity),
                           g.world.stations[ev->sell.origin_station].name,
                           total, currency);
            } else {
                set_notice("%s intake accepted %u %s // +%d %s",
                           dest->name, (unsigned)ev->sell.quantity,
                           commodity_name((commodity_t)ev->sell.commodity),
                           total, currency);
            }
        }
    }
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
    if (amount > 0) spawn_damage_fx(&LOCAL_PLAYER.ship->pos, amount);
    g.damage_flash_timer = 0.4f;
    /* Directional indicator — chevron at the screen edge pointing at
     * the threat. Source = (0,0) means "unknown" (legacy / environmental);
     * skip the indicator for those so it doesn't flicker at world origin. */
    if (ev->damage.source_x != 0.0f || ev->damage.source_y != 0.0f) {
        float dx = ev->damage.source_x - LOCAL_PLAYER.ship->pos.x;
        float dy = ev->damage.source_y - LOCAL_PLAYER.ship->pos.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > 1.0f) {
            g.damage_dir_x = dx / d;
            g.damage_dir_y = dy / d;
            g.damage_dir_timer = 1.5f;
        }
    }
}

bool client_local_public_actor_id(public_actor_id_t *out) {
    actor_principal_t principal = actor_principal_none();
    if (out) *out = public_actor_id_none();
    return out && g.identity_ready &&
        actor_principal_from_stable_id(
            ACTOR_PRINCIPAL_PLAYER, g.identity.pubkey, &principal) &&
        public_actor_id_from_principal(&principal, out);
}

static void sim_on_npc_kill(const sim_event_t *ev) {
    /* Callsigns/slots are presentation hints; source_actor is identity. */
    const char *role = (ev->npc_kill.npc_role == NPC_ROLE_MINER) ? "Miner"
                     : (ev->npc_kill.npc_role == NPC_ROLE_HAULER) ? "Hauler"
                     : "Worker";
    const char *weapon = (ev->npc_kill.cause == DEATH_CAUSE_THROWN_ROCK) ? "thrown rock"
                       : (ev->npc_kill.cause == DEATH_CAUSE_RAM) ? "ramming"
                       : "collision";
    public_actor_id_t local_actor = public_actor_id_none();
    (void)client_local_public_actor_id(&local_actor);
    const char *local_label =
        LOCAL_PLAYER.callsign[0] ? LOCAL_PLAYER.callsign : "YOU";
    client_scoreboard_event_result_t scoreboard_result =
        client_scoreboard_record_npc_kill(
            &g.scoreboard,
            &local_actor,
            local_label,
            &ev->source_actor);
    bool you_killed = scoreboard_result.source_is_local;
    if (you_killed) {
        snprintf(g.kill_feed_text, sizeof(g.kill_feed_text),
                 "You killed %s with %s", role, weapon);
    } else {
        snprintf(g.kill_feed_text, sizeof(g.kill_feed_text),
                 "%s killed by %s", role, weapon);
    }
    g.kill_feed_timer = 3.0f;

    /* NPC victims remain explicitly unattributed until they have a
     * persisted non-secret birth ID. Only the verified killer gets a row. */
    if (you_killed) {
        g.kill_count_session++;
        snprintf(g.kill_confirm_text, sizeof(g.kill_confirm_text),
                 "KILL: %s", role);
        g.kill_confirm_timer = 3.0f;
        audio_play_kill_confirm(&g.audio);
    }
}

static void sim_on_contract_complete(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    if (ev->contract_complete.action == CONTRACT_TRACTOR) {
        set_notice("Haul complete: cargo accepted, payout posted, station demand cooled.");
        episode_trigger(&g.episode, 6); /* Ep 6: Hauler */
    } else if (ev->contract_complete.action == CONTRACT_FRACTURE) {
        set_notice("Bounty complete: asteroid broken, station memory updated.");
    } else if (ev->contract_complete.action == CONTRACT_DELIVERY) {
        set_notice("Delivery complete: cargo accepted, payout posted, route trust increased.");
        char story_notice[192];
        if (story_runtime_mark_delivery(story_notice,
                                        sizeof(story_notice))) {
            set_notice("%s", story_notice);
        }
    }
}

static void sim_on_scaffold_ready(const sim_event_t *ev) {
    int sidx = ev->scaffold_ready.station;
    int mtype = ev->scaffold_ready.module_type;
    if (sidx < 0 || sidx >= MAX_STATIONS) return;
    set_notice("%s scaffold ready at %s. Tow it to extend station function.",
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
    set_notice("Outpost blueprint placed. Bring relay material here to turn fringe space into signal.");
    char story_notice[192];
    if (story_runtime_mark_outpost_placed(story_notice,
                                          sizeof(story_notice))) {
        set_notice("%s", story_notice);
    }
}

/* Spawn the 8 shards + cinematic state for a death event. */
static void death_cinematic_spawn(const sim_event_t *ev) {
    float impact_speed = sqrtf(ev->death.vel_x * ev->death.vel_x +
                               ev->death.vel_y * ev->death.vel_y);
    float severity = clampf(impact_speed / 260.0f, 0.8f, 2.4f);
    uint32_t spin_seed = ((uint32_t)ev->death.respawn_station << 24) ^
                         ((uint32_t)ev->death.cause << 16) ^
                         ((ev->source_actor.kind ==
                           (uint8_t)PUBLIC_ACTOR_ID_DERIVED)
                              ? ((uint32_t)ev->source_actor.id[0] << 8)
                              : 0u) ^
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
    LOCAL_PLAYER.ship->tractor_active = false;
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
    public_actor_id_t local_actor = public_actor_id_none();
    (void)client_local_public_actor_id(&local_actor);
    const char *local_label =
        LOCAL_PLAYER.callsign[0] ? LOCAL_PLAYER.callsign : "YOU";
    client_scoreboard_event_result_t scoreboard_result =
        client_scoreboard_record_death(
            &g.scoreboard,
            &local_actor,
            local_label,
            &ev->source_actor,
            &ev->subject_actor);
    bool victim_is_local = scoreboard_result.subject_is_local;
    bool i_killed_them =
        scoreboard_result.source_is_local && !victim_is_local;
    if (i_killed_them) {
        g.kill_count_session++;
        snprintf(g.kill_confirm_text, sizeof(g.kill_confirm_text),
                 "%s", "KILL: Pilot");
        g.kill_confirm_timer = 3.0f;
        audio_play_kill_confirm(&g.audio);
    }

    if (!victim_is_local) return;
    g.death_count_session++;
    /* Under network authority the cinematic + payload come via
     * NET_MSG_DEATH (only the victim receives that). The broadcast
     * SIM_EVENT_DEATH stream exists only for kill-confirm + scoreboard
     * attribution and carries zeroed cinematic fields, so don't try to
     * render off it here. */
    if (g.net_authority_enabled) return;
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
    episode_reset(&g.episode);
    episode_clear_watched(&g.episode);
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

static bool local_station_balance_for_player(int station_idx, float *out) {
    if (out) *out = 0.0f;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    if (g.net_authority_enabled) {
        int local_station = LOCAL_PLAYER.docked
            ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station;
        if (station_idx == local_station) {
            if (out) *out = g.station_balance;
            return true;
        }
        for (int i = 0; i < g.known_station_ledger_count; i++) {
            const NetKnownLedgerEntry *entry = &g.known_station_ledger[i];
            if (entry->station != (uint8_t)station_idx) continue;
            if (out) *out = entry->balance;
            return true;
        }
        return false;
    }
    const station_t *st = &g.world.stations[station_idx];
    if (!station_exists(st)) return false;
    uint8_t pseudo[32];
    client_session_pseudo_pubkey(LOCAL_PLAYER.session_token, pseudo);
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_pubkey, pseudo, 32) == 0) {
            if (out) *out = st->ledger[i].balance;
            return true;
        }
    }
    return false;
}

static bool local_player_has_other_station_credit(int current_station) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == current_station) continue;
        float bal = 0.0f;
        if (local_station_balance_for_player(s, &bal) && bal > 0.5f)
            return true;
    }
    return false;
}

static int local_player_best_other_station_credit(int current_station,
                                                  float *out_balance)
{
    if (out_balance) *out_balance = 0.0f;
    int best_station = -1;
    float best_balance = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == current_station) continue;
        float bal = 0.0f;
        if (!local_station_balance_for_player(s, &bal)) continue;
        if (bal > best_balance) {
            best_balance = bal;
            best_station = s;
        }
    }
    if (out_balance) *out_balance = best_balance;
    return best_balance > 0.5f ? best_station : -1;
}

static void maybe_notice_local_credits_rule(int station_idx, float balance) {
    if (g.local_credit_hint_shown) return;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return;
    if (balance > 0.5f) return;
    if (!local_player_has_other_station_credit(station_idx)) return;
    g.local_credit_hint_shown = true;
    float other_balance = 0.0f;
    int other_station = local_player_best_other_station_credit(
        station_idx, &other_balance);
    if (other_station >= 0) {
        set_notice("Credits stay local: spend %d at %s on cargo, then haul it here.",
                   (int)lroundf(other_balance),
                   g.world.stations[other_station].name);
    } else {
        set_notice("Credits stay where you earn them. Carry goods, not money.");
    }
}

const char *module_consequence_label(module_type_t type) {
    switch (type) {
    case MODULE_DOCK:
        return "Dock online -- station accepts traffic here.";
    case MODULE_HOPPER:
        return "Hopper online -- ore intake and paired production unlocked.";
    case MODULE_FURNACE:
        return "Furnace online -- smelting can turn fragments into ingots.";
    case MODULE_REPAIR_BAY:
        return "Repair bay online -- hull service available here.";
    case MODULE_SIGNAL_RELAY:
        return "Signal relay online -- civilization reaches farther.";
    case MODULE_FRAME_PRESS:
        return "Frame press online -- ingots can become station frames.";
    case MODULE_LASER_FAB:
        return "Laser fab online -- laser modules can be built here.";
    case MODULE_TRACTOR_FAB:
        return "Tractor fab online -- tractor modules can be built here.";
    case MODULE_ENGINE_FAB:
        return "Engine fab online -- propulsion modules can be built here.";
    case MODULE_SHIPYARD:
        return "Shipyard online -- ships and scaffold kits can be ordered.";
    default:
        return NULL;
    }
}

static bool sim_hail_reason_text(const sim_event_t *ev,
                                 char *out,
                                 size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!ev || ev->hail_response.decision_flags == 0u) return false;

    switch ((hail_decision_mode_t)ev->hail_response.decision_mode) {
    case HAIL_DECISION_MODE_DOCKED:
        snprintf(out, cap, "station knows you are docked");
        return true;
    case HAIL_DECISION_MODE_DOCK_RANGE:
        snprintf(out, cap, "dock signal has priority");
        return true;
    case HAIL_DECISION_MODE_SIGNAL_RANGE:
        if (ev->hail_response.decision_candidate_count > 1u) {
            snprintf(out, cap, "closest of %u station signals",
                     (unsigned)ev->hail_response.decision_candidate_count);
        } else {
            snprintf(out, cap, "only station signal in range");
        }
        return true;
    case HAIL_DECISION_MODE_NONE:
    default:
        if (ev->hail_response.decision_candidate_count == 0u) {
            snprintf(out, cap, "no station signal answered");
            return true;
        }
        break;
    }
    return false;
}

static void sim_on_hail_response(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    char why[96];
    bool has_reason = sim_hail_reason_text(ev, why, sizeof(why));
    int hs = ev->hail_response.station;
    if (hs < 0 || hs >= MAX_STATIONS) {
        if (has_reason)
            set_notice("Local scan sweep. %s.", why);
        else
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
        if (has_reason) {
            set_notice("%s: %s  (%s; balance %d %s)",
                       g.hail_station, g.hail_message, why,
                       (int)lroundf(shown_credits), unit);
        } else {
            set_notice("%s: %s  (balance %d %s)",
                       g.hail_station, g.hail_message,
                       (int)lroundf(shown_credits), unit);
        }
        maybe_notice_local_credits_rule(hs, shown_credits);
    } else {
        if (has_reason)
            set_notice("%s: %s  (%s)", g.hail_station,
                       g.hail_message, why);
        else
            set_notice("%s: %s", g.hail_station, g.hail_message);
    }
    char step[192];
    if (contract_objective_track_contract(ev->hail_response.contract_index,
                                          step, sizeof(step))) {
        set_notice("Tracking: %s", step);
    }
    onboarding_mark_hailed();

    char story_notice[192];
    if (story_runtime_mark_hail(hs, story_notice, sizeof(story_notice)))
        set_notice("%s", story_notice);

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
    const char *consequence =
        module_consequence_label((module_type_t)ev->module_activated.module_type);
    if (consequence)
        set_notice("%s", consequence);
    else
        set_notice("%s online.", module_name);
}

static void sim_on_outpost_activated(const sim_event_t *ev) {
    if (!episode_was_watched(&g.episode, 4))
        episode_trigger(&g.episode, 4); /* Ep 4: Naming */
    audio_play_commission(&g.audio);
    set_notice("Outpost online: local signal expanded. Add modules to make the stop useful.");
    char story_notice[192];
    if (story_runtime_mark_outpost_active(ev->outpost_activated.slot,
                                          story_notice,
                                          sizeof(story_notice))) {
        set_notice("%s", story_notice);
    }
}

static void sim_on_npc_spawned(const sim_event_t *ev) {
    /* Ep 5: Drones — first miner at a player outpost */
    if (!episode_was_watched(&g.episode, 5) &&
        ev->npc_spawned.role == NPC_ROLE_MINER &&
        ev->npc_spawned.home_station >= SIGNAL_FIRST_OUTPOST_INDEX)
        episode_trigger(&g.episode, 5);
}

static void sim_on_signal_lost(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    if (!episode_was_watched(&g.episode, 7))
        episode_trigger(&g.episode, 7); /* Ep 7: Dark Sector */
    char story_notice[192];
    if (story_runtime_mark_signal_gap(story_notice,
                                      sizeof(story_notice))) {
        set_notice("%s", story_notice);
    }
}

static void sim_on_station_connected(const sim_event_t *ev) {
    if (!episode_was_watched(&g.episode, 8) &&
        ev->station_connected.connected_count >= 5)
        episode_trigger(&g.episode, 8); /* Ep 8: Every AI Dreams */
    if (ev->station_connected.connected_count > 0) {
        set_notice("Signal chain connected: %d stations now share route memory.",
                   ev->station_connected.connected_count);
    }
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
    case ORDER_REJECT_POD_PRESENT_STALE:     return "That pod changed after selection -- inspect it and try again.";
    case ORDER_REJECT_POD_PRESENT_NOT_CARRIED:
        return "Tow the selected pod to this dock before unpacking it.";
    case ORDER_REJECT_POD_PRESENT_WRONG_ORIGIN:
        return "This dock cannot originate receipts for cargo made at another station.";
    case ORDER_REJECT_POD_PRESENT_UNTRUSTED:
        return "Pod cargo does not match this station's verified production history.";
    case ORDER_REJECT_POD_PRESENT_STORAGE:
        return "Your ship could not stage the pod manifest -- try again after freeing cargo.";
    case ORDER_REJECT_POD_PRESENT_LOG:
        return "This station's receipt log is unavailable -- pod cargo was left untouched.";
    case ORDER_REJECT_POD_PRESENT_CUSTODY:
        return "This pod's shipment or station custody must be resolved before unpacking.";
    case ORDER_REJECT_POD_PRESENT_IDENTITY:
        return "A verified player identity is required to receive cargo receipts.";
    default:                                return "Order rejected.";
    }
}

static void sim_on_order_rejected(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
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
    if (!g.onboarding.tractored && LOCAL_PLAYER.ship->towed_count > 0)
        onboarding_mark_tractored();
    if (LOCAL_PLAYER.docked && g.onboarding.earned)
        onboarding_mark_docked_after_earning();
    if (LOCAL_PLAYER.docked &&
        g.station_view == STATION_VIEW_TRADE &&
        g.onboarding.docked_after_earning) {
        onboarding_mark_viewed_trade();
    }
}

static void episode_per_frame(float dt) {
    if (episode_is_active(&g.episode)) return;

    if (g.deferred_episode_mask != 0u && LOCAL_PLAYER.docked &&
        g.onboarding.complete) {
        if (g.deferred_episode_timer > 0.0f) {
            g.deferred_episode_timer = fmaxf(
                0.0f, g.deferred_episode_timer - fmaxf(0.0f, dt));
        } else {
            for (int i = 0; i < EPISODE_COUNT; i++) {
                uint16_t bit = (uint16_t)(1u << i);
                if ((g.deferred_episode_mask & bit) == 0u) continue;
                g.deferred_episode_mask &= (uint16_t)(UINT16_MAX ^ bit);
                episode_trigger(&g.episode, i);
                break;
            }
        }
    }
    if (episode_is_active(&g.episode)) return;

    /* Ep 3: Scaffold — currently towing a scaffold */
    if (!episode_was_watched(&g.episode, 3) &&
        LOCAL_PLAYER.ship->towed_scaffold >= 0)
        episode_trigger(&g.episode, 3);

    /* Ep 4, 5, 7, 8 are now event-driven (see process_events) */
}

static void sim_step(float dt) {
    reset_step_feedback();
    audio_step(&g.audio, dt);

    /* Advance world time locally under network authority.
     * Ring rotations are authoritative server-side; client prediction
     * integrates omega each frame, while net_sync eases any phase correction
     * from the latest station-identity snapshot instead of snapping. */
    if (g.net_authority_enabled) {
        g.world.time += dt;
        step_remote_station_rings(dt);
    }

    if (g.local_server.active && net_is_loopback()) {
        local_server_step_loopback(&g.local_server, g.local_player_slot, dt);
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
            g.inspect_receipt_browser = false;
            g.inspect_receipt_page = 0;
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
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
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
        float frac = LOCAL_PLAYER.ship->hull / fmaxf(1.0f, ship_max_hull(LOCAL_PLAYER.ship));
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
        LOCAL_PLAYER.ship->tractor_active = false;
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
        LOCAL_PLAYER.ship->vel = v2(0.0f, 0.0f);

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

    input_intent_t intent = {0};
    if (!legacy_recovery_ui_blocks_gameplay(&legacy_recovery_ui)) {
        intent = sample_input_intent();
    }

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
    if (neural_singleplayer_ready()) {
        const world_t *shadow_world = &g.world;
        const server_player_t *shadow_player = &LOCAL_PLAYER;
        const world_t *authority =
            local_server_world_const(&g.local_server);
        if (g.local_server.active &&
            authority &&
            net_is_loopback() &&
            g.local_player_slot >= 0 &&
            g.local_player_slot < MAX_PLAYERS) {
            shadow_world = authority;
            shadow_player = &authority->players[g.local_player_slot];
        }
        neural_singleplayer_shadow_flight(
            shadow_world, shadow_player, &intent, NULL);
    }

    /* Version mismatch: reload once to get matching client.
     * Only reload if we haven't already tried (check ?v= in URL).
     * deploy-client runs before deploy-server, so the new client
     * is on CDN by the time the new server sends its hash. */
    if (g.net_authority_enabled && net_is_connected()) {
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
    net_advance_asteroid_interpolation(dt);
    net_advance_npc_interpolation(dt);
    net_advance_scaffold_interpolation(dt);
    net_advance_cargo_pod_interpolation(dt);
    g.player_interp.t += dt / fmaxf(g.player_interp.interval, 0.01f);

    /* Thrust flame: local input for manual, server input for autopilot.
     * PLAYER_STATE flags carry bit0=thrust, decoded into
     * g.server_thrusting. */
    if (LOCAL_PLAYER.autopilot_mode) {
        g.thrusting = g.server_thrusting && !LOCAL_PLAYER.docked;
    } else {
        g.thrusting = (intent.thrust > 0.0f) && !LOCAL_PLAYER.docked;
    }

    /* Play audio + trigger UI from sim events from the authoritative stream. */
    process_sim_events(&g.world.events);
    g.world.events.count = 0;  /* consume — don't replay on next sim step */

    /* Detect state transitions for music/episode triggers (works in both modes).
     * Must run BEFORE was_docked is updated to detect the transition. */
    if (g.was_docked && !LOCAL_PLAYER.docked) {
        /* Just launched */
        LOCAL_PLAYER.in_dock_range = false;
        LOCAL_PLAYER.docking_approach = false;
        LOCAL_PLAYER.nearby_station = -1;
        g.camera_pos = LOCAL_PLAYER.ship->pos;
        g.camera_initialized = true;
        g.camera_station_index = -1;
        g.camera_station_side = 0;
        g.camera_station_v_side = 0;
        g.camera_drift_timer = 0.0f;
        g.local_player_render_offset = v2(0.0f, 0.0f);
        if (g.onboarding.complete)
            episode_trigger(&g.episode, 0);
        else
            defer_episode_until_docked(0);
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
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
        if (sig < SIGNAL_BAND_OPERATIONAL)
            set_notice("Autopilot disengaged -- weak signal.");
    }
    g.was_autopilot = LOCAL_PLAYER.autopilot_mode;

    /* Death cinematic payload arrives through NET_MSG_DEATH in both remote
     * remote WebSocket and local loopback; SIM_EVENT_DEATH still drives shared
     * scoreboard/kill attribution. */

    /* Update was_docked AFTER transition checks */
    g.was_docked = LOCAL_PLAYER.docked;

    onboarding_per_frame();
    episode_per_frame(dt);
    episode_update(&g.episode, dt);
    music_update(&g.music, dt);
    {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
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
    if (g.input.key_pressed[SAPP_KEYCODE_F3]) {
        g.hud_debug_visible = !g.hud_debug_visible;
        set_notice(g.hud_debug_visible
            ? "Network/build telemetry shown."
            : "Network/build telemetry hidden.");
    }

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
        int now_towed = LOCAL_PLAYER.ship->towed_count;
        if (now_towed > prev_towed) audio_play_tractor_lock(&g.audio);
        prev_towed = now_towed;
    }
    /* Tab while undocked opens/pages visible inspect provenance first. Without
     * an active scan pane it keeps its older session-scoreboard behavior.
     * Docked Tab is already taken for station panels. */
    bool inspect_surface_active =
        g.inspect_station >= 0 ||
        (g.inspect_snapshot_timer > 0.0f &&
         g.inspect_snapshot.target_type != INSPECT_TARGET_NONE);
    if (inspect_surface_active)
        g.scoreboard.show = false;
    if (!LOCAL_PLAYER.docked && g.input.key_pressed[SAPP_KEYCODE_TAB]) {
        if (g.inspect_snapshot_timer > 0.0f &&
            g.inspect_snapshot.target_type != INSPECT_TARGET_NONE) {
            bool shift = g.input.key_down[SAPP_KEYCODE_LEFT_SHIFT] ||
                         g.input.key_down[SAPP_KEYCODE_RIGHT_SHIFT];
            if (shift && g.inspect_receipt_browser) {
                g.inspect_receipt_browser = false;
            } else if (!g.inspect_receipt_browser) {
                g.inspect_receipt_browser = true;
                g.inspect_receipt_page = 0;
            } else {
                g.inspect_receipt_page++;
            }
        } else {
            g.scoreboard.show = !g.scoreboard.show;
        }
    }
    if (g.action_predict_timer > 0.0f)
        g.action_predict_timer = fmaxf(0.0f, g.action_predict_timer - dt);
    if (g.dock_settle_timer > 0.0f)
        g.dock_settle_timer = fmaxf(0.0f, g.dock_settle_timer - dt);

    consume_pressed_input();
}

/* on_player_join ... sync_local_player_slot_from_network: see net_sync.h/c */

static void init(void) {
    /*
     * `g` has static storage and enters the sole application init callback
     * zero-initialized.  Avoid eagerly touching the entire multi-megabyte
     * client state before mode selection; reset_world() initializes the
     * replicated world below and each subsystem owns its remaining setup.
     */
    g.world.rng = 0xC0FFEE12u;

    bool jank_profile_enabled = false;
#ifdef __EMSCRIPTEN__
    jank_profile_enabled = emscripten_run_script_int(
        "new URLSearchParams(location.search).get('jankprofile')==='1'") != 0;
#else
    const char *jank_profile = getenv("SIGNAL_JANK_PROFILE");
    jank_profile_enabled = jank_profile && jank_profile[0] != '\0' &&
        strcmp(jank_profile, "0") != 0;
#endif
    gameplay_observability_configure(jank_profile_enabled, 60.0);

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
    g.identity_ready = identity_load_or_generate(&g.identity);
    if (g.identity_ready) {
        base58_encode(g.identity.pubkey,
                      SIGNAL_CRYPTO_PUBKEY_BYTES,
                      g.identity_pub_b58,
                      sizeof(g.identity_pub_b58));
    } else {
        memset(&g.identity, 0, sizeof(g.identity));
        g.identity_pub_b58[0] = '\0';
        fprintf(stderr,
                "[identity] secure bootstrap unavailable; "
                "network authentication disabled\n");
    }

    mining_client_init();
    /* Bind to whatever session token the bootstrap world seeded. Remote
     * WebSocket connect rebinds to the authoritative token once the
     * handshake completes. */
    mining_client_set_session_token(g.world.players[g.local_player_slot].session_token);

    /* --- Network authority: remote server URL when present, local loopback otherwise. --- */
    {
        const char* server_url = NULL;
#ifdef __EMSCRIPTEN__
        server_url = emscripten_run_script_string(
            "(() => {"
            "  const p = new URLSearchParams(window.location.search);"
            "  const server = p.get('server') || window.SIGNAL_SERVER || '';"
            "  if (p.has('singleplayer')) return '';"
            "  return server;"
            "})()");
#else
        /* Native: check SIGNAL_SERVER environment variable or command line */
        server_url = getenv("SIGNAL_SERVER");
#endif
        client_progress_select(g.identity_ready ? g.identity.pubkey : NULL,
                               server_url);
        onboarding_load();
        story_runtime_load();
        signal_intelligence_holographic_init();
        {
            NetCallbacks cbs;
            configure_net_callbacks(&cbs);
            net_set_identity_pubkey(
                g.identity_ready ? g.identity.pubkey : NULL);
            net_set_identity_secret(
                g.identity_ready ? g.identity.secret : NULL);
            bool remote_requested = server_url && server_url[0] != '\0';
            if (remote_requested) {
                bool remote_started = net_init(server_url, &cbs);
                /* Keep the requested authority mode even when transport
                 * setup fails. Falling through to local authority here
                 * creates a different world while presenting it as a
                * continuation of multiplayer. The offline frame path can
                 * now offer an honest reconnect/reload instead. */
                g.net_authority_enabled = true;
                /* A failed first connection must not leave the seeded client
                 * bootstrap world visible as if it were multiplayer. */
                reset_remote_dynamic_sync();
                if (!remote_started) {
                    set_notice("Remote unavailable. Press [P] to reconnect.");
                }
            }
            if (!remote_requested) {
                g.net_authority_enabled =
                    start_fresh_local_authority(&cbs);
                if (!g.net_authority_enabled) {
                    set_notice(
                        "Local authority unavailable (out of memory).");
                }
            }
        }
    }
}


/* on_player_join ... sync_local_player_slot_from_network: see net_sync.h/c */

static void render_world(void) {
#ifdef __EMSCRIPTEN__
    /* Render fixtures must be applied after any pending authoritative packet
     * and before camera/frustum calculation, not only when a HUD getter is
     * queried by browser tests. */
    smoke_apply_loop_state_for_frame();
#endif
    /* Guard against Safari's NaN-on-audio-resume frame (the same one
     * ui_window_width handles). Unguarded NaN here propagates through
     * set_camera_bounds into cam_right - cam_left, then into
     * draw_callsigns/draw_npc_chatter's sdtx_canvas call, which
     * asserts !isnan(w). See hud.h ui_safe_positive comment. */
    float win_w = ui_safe_positive(sapp_widthf(), 1280.0f);
    float win_h = ui_safe_positive(sapp_heightf(), 720.0f);
    float half_w = win_w * 0.5f;
    float half_h = win_h * 0.5f;
    float narrow_focus = camera_view_narrow_focus(win_w, win_h);
    /* Camera modes:
     *   1. Death cinematic — anchor to wreckage, mild damping
     *   2. Station encounter — lock the station to one side of the screen
     *      (left or right) based on which way the player approached
     *   3. Free flight — DEADZONE camera. The ship moves freely inside
     *      a center deadzone. When it hits the edge of the deadzone the
     *      camera latches to that edge and follows. Sustained high-speed
     *      motion lazily recenters the camera onto the ship. */
    if (!g.camera_initialized) {
        g.camera_pos = LOCAL_PLAYER.ship->pos;
        g.camera_initialized = true;
        g.boost_zoom = 1.0f;
        g.cargo_focus_zoom = 1.0f;
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
        bool cargo_handoff = !LOCAL_PLAYER.docked &&
            ship_towed_pod_count(LOCAL_PLAYER.ship) > 0 &&
            LOCAL_PLAYER.nearby_station >= 0;
        float target_cargo_zoom = cargo_handoff ? 0.84f : 1.0f;
        /* Slower ease-in than ease-out so the zoom feels like it locks
         * on gradually while active, and release is equally gentle. */
        float kz = 1.0f - expf(-cdt / 0.9f);
        float kb = 1.0f - expf(-cdt / 0.7f);
        g.boost_zoom         += (target_zoom  - g.boost_zoom)         * kz;
        g.cargo_focus_zoom   += (target_cargo_zoom - g.cargo_focus_zoom) * kz;
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
                g.camera_station_side = (LOCAL_PLAYER.ship->pos.x <= anchor_station->pos.x) ? +1 : -1;
                g.camera_station_v_side = (LOCAL_PLAYER.ship->pos.y <= anchor_station->pos.y) ? +1 : -1;
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
            vec2 ship = LOCAL_PLAYER.ship->pos;
            float dz_x = half_w * camera_deadzone_x_scale(narrow_focus);
            float dz_y = half_h * camera_deadzone_y_scale(narrow_focus);
            float dx = ship.x - g.camera_pos.x;
            float dy = ship.y - g.camera_pos.y;

            if (fabsf(dx) > half_w * 0.85f || fabsf(dy) > half_h * 0.85f) {
                g.camera_pos = ship;
                g.camera_drift_timer = 0.0f;
                g.local_player_render_offset = v2(0.0f, 0.0f);
                dx = 0.0f;
                dy = 0.0f;
            }

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
            float speed_sq = LOCAL_PLAYER.ship->vel.x * LOCAL_PLAYER.ship->vel.x
                           + LOCAL_PLAYER.ship->vel.y * LOCAL_PLAYER.ship->vel.y;
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
                vec2 mid = v2(0.5f * (LOCAL_PLAYER.ship->pos.x + tn->ship->pos.x),
                              0.5f * (LOCAL_PLAYER.ship->pos.y + tn->ship->pos.y));
                float strength = g.inspect_was_active
                                 ? 1.0f
                                 : (g.inspect_snapshot_timer < 1.0f
                                    ? g.inspect_snapshot_timer : 1.0f);
                float k = (1.0f - expf(-2.5f * dt)) * 0.6f * strength;
                g.camera_pos.x += (mid.x - g.camera_pos.x) * k;
                g.camera_pos.y += (mid.y - g.camera_pos.y) * k;
            }
        }

        if (!g.death_cinematic.active && narrow_focus > 0.001f) {
            float strength = camera_narrow_center_strength(narrow_focus);
            float k = (1.0f - expf(-4.0f * dt)) * strength;
            vec2 ship = LOCAL_PLAYER.ship->pos;
            g.camera_pos.x += (ship.x - g.camera_pos.x) * k;
            g.camera_pos.y += (ship.y - g.camera_pos.y) * k;
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

    /* Composed camera zoom. Hail widens; boost or a physical cargo handoff
     * tightens. The two close-focus modes use the tighter value rather than
     * multiplying, which avoids an over-zoom when boosting a crate into an
     * intake. */
    float ping_zoom = hail_ping_camera_zoom();
    float focus_zoom = fminf(g.boost_zoom, g.cargo_focus_zoom);
    float total_zoom = ping_zoom * focus_zoom;
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
    draw_cargo_pods();
    draw_towed_cargo_hopper_guides();

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
            draw_segment(LOCAL_PLAYER.ship->pos, mp, 0.2f, 0.8f, 1.0f, tp * 0.3f);
            /* Info text near module (world-space debugtext) */
            float screen_w = ui_screen_width();
            float screen_h = ui_screen_height();
            sdtx_canvas(screen_w, screen_h);
            sdtx_origin(0, 0);
            /* Convert world pos to screen pos */
            vec2 cam = LOCAL_PLAYER.ship->pos;
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
                case MODULE_ENGINE_FAB:  sell_c = COMMODITY_ENGINE_MODULE; break;
                default: break;
            }
            if ((int)sell_c >= 0) {
                int stock = (int)lroundf(
                    client_station_stock_amount(tst, sell_c));
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
                tst, g.target_module, g.net_authority_enabled && net_is_connected());
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

    /*
     * Recovery is a provisional authenticated dock console, not gameplay
     * station state. Draw it alone so underlying HUD text and controls cannot
     * bleed through the scrim before the atomic decision completes.
     */
    if (legacy_recovery_ui_visible(&legacy_recovery_ui)) {
        draw_legacy_recovery_ui(
            &legacy_recovery_ui, net_now_ms32());
        return;
    }

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
    if (!g.net_authority_enabled || g.death_cinematic.active ||
        LOCAL_PLAYER.docked) {
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

static float local_player_render_extrapolation_dt(void) {
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS)
        return 0.0f;
    if (g.death_cinematic.active || LOCAL_PLAYER.docked)
        return 0.0f;

    float t = g.runtime.accumulator;
    if (!isfinite(t) || t <= 0.0f) return 0.0f;
    if (t > SIM_DT) t = SIM_DT;
    return t;
}

typedef struct {
    int index;
    vec2 pos;
    float rotation;
    float age;
} local_asteroid_render_pose_t;

typedef struct {
    int index;
    vec2 pos;
    float rotation;
    float age;
} local_cargo_pod_render_pose_t;

typedef struct {
    int index;
    vec2 pos;
    float rotation;
    float age;
} local_scaffold_render_pose_t;

static vec2 local_towed_body_render_pos(vec2 pos, vec2 vel,
                                        vec2 shared_offset,
                                        float render_ahead) {
    vec2 render_pos = v2_add(pos, shared_offset);
    if (render_ahead > 0.0f)
        render_pos = v2_add(render_pos, v2_scale(vel, render_ahead));
    return render_pos;
}

static int apply_local_towed_asteroid_render_pose(
    local_asteroid_render_pose_t saves[10],
    vec2 shared_offset,
    float render_ahead)
{
    int count = 0;
    int tow_count = LOCAL_PLAYER.ship->towed_count;
    if (tow_count > 10) tow_count = 10;

    for (int t = 0; t < tow_count; t++) {
        int idx = LOCAL_PLAYER.ship->towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || count >= 10) continue;
        asteroid_t *a = &g.world.asteroids[idx];
        if (!a->active) continue;

        saves[count++] = (local_asteroid_render_pose_t){
            .index = idx,
            .pos = a->pos,
            .rotation = a->rotation,
            .age = a->age,
        };
        a->pos = local_towed_body_render_pos(a->pos, a->vel,
                                             shared_offset, render_ahead);
        if (render_ahead > 0.0f) {
            a->rotation = wrap_angle(a->rotation + a->spin * render_ahead);
            a->age += render_ahead;
        }
    }
    return count;
}

static int apply_local_towed_cargo_pod_render_pose(
    local_cargo_pod_render_pose_t saves[10],
    vec2 shared_offset,
    float render_ahead)
{
    int count = 0;
    int tow_count = LOCAL_PLAYER.ship->towed_pod_count;
    if (tow_count > 10) tow_count = 10;

    for (int t = 0; t < tow_count; t++) {
        int idx = LOCAL_PLAYER.ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS || count >= 10) continue;
        cargo_pod_t *pod = &g.world.cargo_pods[idx];
        if (!pod->active) continue;

        saves[count++] = (local_cargo_pod_render_pose_t){
            .index = idx,
            .pos = pod->pos,
            .rotation = pod->rotation,
            .age = pod->age,
        };
        pod->pos = local_towed_body_render_pos(pod->pos, pod->vel,
                                               shared_offset, render_ahead);
        if (render_ahead > 0.0f) {
            pod->rotation = wrap_angle(pod->rotation + pod->spin * render_ahead);
            pod->age += render_ahead;
        }
    }
    return count;
}

static bool apply_local_towed_scaffold_render_pose(
    local_scaffold_render_pose_t *save,
    vec2 shared_offset,
    float render_ahead)
{
    int idx = LOCAL_PLAYER.ship->towed_scaffold;
    if (idx < 0 || idx >= MAX_SCAFFOLDS || !save) return false;
    scaffold_t *sc = &g.world.scaffolds[idx];
    if (!sc->active) return false;

    *save = (local_scaffold_render_pose_t){
        .index = idx,
        .pos = sc->pos,
        .rotation = sc->rotation,
        .age = sc->age,
    };
    sc->pos = local_towed_body_render_pos(sc->pos, sc->vel,
                                          shared_offset, render_ahead);
    if (render_ahead > 0.0f) {
        sc->rotation = wrap_angle(sc->rotation + sc->spin * render_ahead);
        sc->age += render_ahead;
    }
    return true;
}

static void restore_local_towed_asteroid_render_pose(
    const local_asteroid_render_pose_t saves[10],
    int count)
{
    for (int i = 0; i < count; i++) {
        int idx = saves[i].index;
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        asteroid_t *a = &g.world.asteroids[idx];
        a->pos = saves[i].pos;
        a->rotation = saves[i].rotation;
        a->age = saves[i].age;
    }
}

static void restore_local_towed_cargo_pod_render_pose(
    const local_cargo_pod_render_pose_t saves[10],
    int count)
{
    for (int i = 0; i < count; i++) {
        int idx = saves[i].index;
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        cargo_pod_t *pod = &g.world.cargo_pods[idx];
        pod->pos = saves[i].pos;
        pod->rotation = saves[i].rotation;
        pod->age = saves[i].age;
    }
}

static void restore_local_towed_scaffold_render_pose(
    const local_scaffold_render_pose_t *save)
{
    if (!save || save->index < 0 || save->index >= MAX_SCAFFOLDS) return;
    scaffold_t *sc = &g.world.scaffolds[save->index];
    sc->pos = save->pos;
    sc->rotation = save->rotation;
    sc->age = save->age;
}

static int g_render_queued_vertices;
static int g_render_queued_commands;
static uint32_t g_render_sgl_error_mask;
static float g_render_frame_duration_ms;

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
int signal_render_queued_vertices(void) {
    return g_render_queued_vertices;
}

EMSCRIPTEN_KEEPALIVE
int signal_render_queued_commands(void) {
    return g_render_queued_commands;
}

EMSCRIPTEN_KEEPALIVE
int signal_render_sgl_error_mask(void) {
    return (int)g_render_sgl_error_mask;
}

EMSCRIPTEN_KEEPALIVE
float signal_render_frame_duration_ms(void) {
    return g_render_frame_duration_ms;
}

EMSCRIPTEN_KEEPALIVE
int signal_jank_profile_enabled(void) {
    return gameplay_observability_enabled() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void signal_jank_profile_reset(void) {
    gameplay_observability_reset();
}

EMSCRIPTEN_KEEPALIVE
const char *signal_jank_profile_report_json(void) {
    return gameplay_observability_report_json();
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_accelerated_asteroid_prediction_gate(void) {
    return asteroid_presentation_acceleration_gate(NULL, NULL) ? 1 : 0;
}
#endif

static void render_frame(void) {
    double interpolation_started = gameplay_observability_phase_begin();
    float frame_dt = (float)sapp_frame_duration();
    if (frame_dt <= 0.0f) frame_dt = 1.0f / 60.0f;
    if (frame_dt > 0.1f) frame_dt = 0.1f;
    interpolate_world_for_render_frame(frame_dt);
    g_render_frame_duration_ms = frame_dt * 1000.0f;
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

    vec2 saved_ship_pos = LOCAL_PLAYER.ship->pos;
    float saved_ship_angle = LOCAL_PLAYER.ship->angle;
    local_asteroid_render_pose_t saved_asteroids[10];
    local_cargo_pod_render_pose_t saved_pods[10];
    local_scaffold_render_pose_t saved_scaffold = { .index = -1 };
    int saved_asteroid_count = 0;
    int saved_pod_count = 0;
    bool saved_scaffold_active = false;
    float render_ahead = local_player_render_extrapolation_dt();
    bool apply_visual_pose =
        render_ahead > 0.0f || v2_len_sq(g.local_player_render_offset) > 0.01f;
    if (apply_visual_pose) {
        vec2 render_pos = v2_add(saved_ship_pos, g.local_player_render_offset);
        render_pos = v2_add(render_pos,
                            v2_scale(LOCAL_PLAYER.ship->vel, render_ahead));
        LOCAL_PLAYER.ship->pos = render_pos;
        if (fabsf(LOCAL_PLAYER.input.turn) > 0.0001f) {
            const hull_def_t *hull = ship_hull_def(LOCAL_PLAYER.ship);
            LOCAL_PLAYER.ship->angle = wrap_angle(
                saved_ship_angle +
                LOCAL_PLAYER.input.turn * hull->turn_speed * render_ahead);
        }
        float asteroid_render_ahead =
            get_local_asteroid_motion_feed_active()
                ? 0.0f : render_ahead;
        saved_asteroid_count = apply_local_towed_asteroid_render_pose(
            saved_asteroids, g.local_player_render_offset,
            asteroid_render_ahead);
        saved_pod_count = apply_local_towed_cargo_pod_render_pose(
            saved_pods, g.local_player_render_offset, render_ahead);
        saved_scaffold_active = apply_local_towed_scaffold_render_pose(
            &saved_scaffold, g.local_player_render_offset, render_ahead);
    }
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_INTERPOLATION, interpolation_started);

    double world_render_started = gameplay_observability_phase_begin();
    render_world();
    if (apply_visual_pose) {
        restore_local_towed_asteroid_render_pose(saved_asteroids,
                                                 saved_asteroid_count);
        restore_local_towed_cargo_pod_render_pose(saved_pods,
                                                  saved_pod_count);
        if (saved_scaffold_active)
            restore_local_towed_scaffold_render_pose(&saved_scaffold);
        LOCAL_PLAYER.ship->pos = saved_ship_pos;
        LOCAL_PLAYER.ship->angle = saved_ship_angle;
    }
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_WORLD_RENDER, world_render_started);

    double ui_render_started = gameplay_observability_phase_begin();
    render_ui();
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_UI_RENDER, ui_render_started);

    g_render_queued_vertices = sgl_num_vertices();
    g_render_queued_commands = sgl_num_commands();
    sgl_error_t render_error = sgl_error();
    g_render_sgl_error_mask =
        (render_error.vertices_full ? 1u << 0 : 0u) |
        (render_error.uniforms_full ? 1u << 1 : 0u) |
        (render_error.commands_full ? 1u << 2 : 0u) |
        (render_error.stack_overflow ? 1u << 3 : 0u) |
        (render_error.stack_underflow ? 1u << 4 : 0u) |
        (render_error.no_context ? 1u << 5 : 0u);

    double submission_started = gameplay_observability_phase_begin();
    sg_begin_pass(&(sg_pass){
        .action = g.pass_action,
        .swapchain = sglue_swapchain(),
    });
    sgl_draw();
    sdtx_draw();
    sg_end_pass();
    sg_commit();
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_SUBMISSION, submission_started);
}

static void advance_simulation_frame(float frame_dt) {
    g.runtime.accumulator += frame_dt;

    double due_f = floor((double)g.runtime.accumulator / (double)SIM_DT);
    uint32_t due = due_f > (double)UINT32_MAX
        ? UINT32_MAX : (uint32_t)due_f;

    int sim_steps = 0;
    while ((g.runtime.accumulator >= SIM_DT) && (sim_steps < MAX_SIM_STEPS_PER_FRAME)) {
        sim_step(SIM_DT);
        g.runtime.accumulator -= SIM_DT;
        sim_steps++;
    }

    uint32_t missed = due > (uint32_t)sim_steps
        ? due - (uint32_t)sim_steps : 0u;
    uint32_t dropped = 0;
    if (g.runtime.accumulator >= SIM_DT) {
        double drop_f = floor(
            (double)g.runtime.accumulator / (double)SIM_DT);
        dropped = drop_f > (double)UINT32_MAX
            ? UINT32_MAX : (uint32_t)drop_f;
        g.runtime.accumulator -= (float)dropped * SIM_DT;
        if (g.runtime.accumulator < 0.0f) g.runtime.accumulator = 0.0f;
    }
    gameplay_observability_record_sim_steps(
        (uint32_t)sim_steps, missed, dropped);
}

/* Exported for the JS music player — returns 0.0-1.0 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_signal_strength(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
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
float get_signal_visual_player_saturation(void) {
    return world_signal_visual_player_saturation();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_camera_offset_x(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->pos.x - g.camera_pos.x;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_camera_offset_y(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->pos.y - g.camera_pos.y;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_pos_x(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->pos.x;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_pos_y(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->pos.y;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_vel_x(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->vel.x;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_vel_y(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->vel.y;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_player_angle(void) {
    if (g.local_player_slot < 0) return 0.0f;
    return LOCAL_PLAYER.ship->angle;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_player_docked(void) {
    if (g.local_player_slot < 0) return 0;
    return LOCAL_PLAYER.docked ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_camera_narrow_focus(void) {
    float win_w = ui_safe_positive(sapp_widthf(), 1280.0f);
    float win_h = ui_safe_positive(sapp_heightf(), 720.0f);
    return camera_view_narrow_focus(win_w, win_h);
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
    net_reconcile_diagnostics_reset(&g.net_reconcile);
    g.net_motion.input_lead_margin_ticks =
        NET_INPUT_LEAD_DEFAULT_MARGIN_TICKS;
    net_latency_stats_reset(&g.net_ack_latency);
    net_latency_stats_reset(&g.net_ping_latency);
    net_latency_gap_stats_reset(&g.net_ack_gap);
    g.net_last_ack_rtt = 0.0f;
    g.net_last_ping_raw_rtt = 0.0f;
    g.net_last_ping_rtt = 0.0f;
    g.net_last_ping_server_turnaround_ms = 0.0f;
    g.net_last_dedicated_ping_sample_time = 0.0f;
    g.net_last_ack_transport_sample_time = 0.0f;
    g.net_max_ping_rtt_5s = 0.0f;
    g.net_ping_samples = 0;
    g.net_missed_pongs = 0;
    g.net_missed_input_acks = 0;
    g.net_ack_recovery_packets = 0;
    g.net_ping_miss_windows_reported = 0;
    g.net_ack_miss_windows_reported = 0;
    g.net_ack_recovery_tier = NET_LATENCY_ACK_RECOVERY_STEADY;
    g.net_max_ack_rtt_5s = 0.0f;
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
float get_net_motion_last_ping_raw_rtt_ms(void) {
    return g.net_last_ping_raw_rtt * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_smoothed_ack_rtt_ms(void) {
    if (!net_latency_stats_fresh(&g.net_ack_latency,
                                 g.net_time,
                                 NET_LATENCY_STALE_SEC)) {
        return 0.0f;
    }
    return net_latency_stats_smoothed_sec(&g.net_ack_latency) * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_smoothed_ping_rtt_ms(void) {
    if (!net_latency_stats_fresh(&g.net_ping_latency,
                                 g.net_time,
                                 NET_LATENCY_STALE_SEC)) {
        return 0.0f;
    }
    return net_latency_stats_smoothed_sec(&g.net_ping_latency) * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_ack_fresh(void) {
    return net_latency_stats_fresh(&g.net_ack_latency,
                                   g.net_time,
                                   NET_LATENCY_STALE_SEC) ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_ping_fresh(void) {
    return net_latency_stats_fresh(&g.net_ping_latency,
                                   g.net_time,
                                   NET_LATENCY_STALE_SEC) ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_last_ack_gap_ms(void) {
    return g.net_ack_gap.count > 0 ? g.net_ack_gap.last * 1000.0f : 0.0f;
}

static void net_count_latency_miss_windows(const net_latency_stats_t *stats,
                                           uint32_t *reported_windows,
                                           uint32_t *total_misses) {
    if (!reported_windows || !total_misses) return;
    uint32_t windows = net_latency_stale_window_miss_count(
        stats, g.net_time, NET_LATENCY_STALE_SEC);
    if (windows < *reported_windows)
        *reported_windows = windows;
    if (windows > *reported_windows) {
        *total_misses += windows - *reported_windows;
        *reported_windows = windows;
    }
}

static void net_update_latency_miss_counters(void) {
    net_count_latency_miss_windows(&g.net_ping_latency,
                                   &g.net_ping_miss_windows_reported,
                                   &g.net_missed_pongs);
    net_count_latency_miss_windows(&g.net_ack_latency,
                                   &g.net_ack_miss_windows_reported,
                                   &g.net_missed_input_acks);
}

static bool net_control_lane_quiet_stable(void) {
    if (g.net_ping_miss_windows_reported > 0 ||
        g.net_ack_miss_windows_reported > 0 ||
        net_unacked_input_count() > NET_CONTROL_LANE_QUIET_MAX_UNACKED) {
        return false;
    }
    return net_latency_control_lane_stable(
        &g.net_ping_latency,
        &g.net_ack_latency,
        g.net_time,
        NET_LATENCY_STALE_SEC,
        NET_CONTROL_LANE_STABLE_GAP_SEC,
        NET_LATENCY_STABLE_MIN_SAMPLES);
}

static bool net_recent_input_ack_transport_sample(void) {
    return g.net_last_ack_transport_sample_time > 0.0f &&
        g.net_time - g.net_last_ack_transport_sample_time <=
            NET_LATENCY_STALE_SEC;
}

static float net_latency_ping_interval_sec(void) {
    bool quiet_stable = net_control_lane_quiet_stable();
    float quiet_interval = quiet_stable
        ? (net_recent_input_ack_transport_sample()
              ? NET_PING_ACK_SAMPLED_QUIET_INTERVAL_SEC
              : NET_PING_QUIET_INTERVAL_SEC)
        : NET_PING_STEADY_INTERVAL_SEC;
    return net_latency_ping_interval_for_state(
        &g.net_ping_latency,
        &g.net_ack_latency,
        g.net_time,
        NET_LATENCY_STALE_SEC,
        g.net_ping_samples,
        NET_PING_BOOT_INTERVAL_SEC,
        NET_PING_RECOVERY_INTERVAL_SEC,
        NET_PING_STEADY_INTERVAL_SEC,
        quiet_interval,
        NET_PING_ACK_GAP_RECOVERY_SEC,
        NET_CONTROL_LANE_STABLE_GAP_SEC,
        NET_LATENCY_STABLE_MIN_SAMPLES);
}

static uint8_t net_active_input_ack_recovery_tier(void) {
    bool ack_stale = g.net_motion.total_input_acks > 0 &&
        !net_latency_stats_fresh(&g.net_ack_latency,
                                 g.net_time,
                                 NET_LATENCY_STALE_SEC);
    float ack_gap = net_latency_gap_stats_fresh(
        &g.net_ack_gap, g.net_time, NET_LATENCY_STALE_SEC)
        ? net_latency_gap_stats_smoothed_sec(&g.net_ack_gap) : 0.0f;
    uint32_t age_recovery_miss =
        net_latency_unacked_age_needs_recovery(
            net_oldest_unacked_input_age_sec(),
            net_prediction_control_rtt_sec(),
            NET_ACTIVE_INPUT_ACK_AGE_RECOVERY_MIN_SEC,
            NET_ACTIVE_INPUT_ACK_AGE_RECOVERY_RTT_MULT) ? 1u : 0u;
    return net_latency_ack_recovery_tier(
        net_unacked_input_count(),
        ack_stale,
        g.net_ack_miss_windows_reported + age_recovery_miss,
        ack_gap,
        NET_ACTIVE_INPUT_ACK_RECOVERY_UNACKED,
        NET_ACTIVE_INPUT_ACK_HOT_RECOVERY_UNACKED,
        NET_ACTIVE_INPUT_ACK_RECOVERY_GAP_SEC,
        NET_ACTIVE_INPUT_ACK_HOT_RECOVERY_GAP_SEC);
}

static float net_active_input_ack_interval_sec(void) {
    g.net_ack_recovery_tier = net_active_input_ack_recovery_tier();
    return NET_ACTIVE_INPUT_ACK_HEARTBEAT_SEC;
}

static uint8_t net_client_recovery_flags(void) {
    uint8_t flags =
        (uint8_t)(g.net_ack_recovery_tier & NET_CLIENT_METRICS_ACK_TIER_MASK);
    if (net_latency_stats_fresh(&g.net_ping_latency,
                                g.net_time,
                                NET_LATENCY_STALE_SEC)) {
        flags |= NET_CLIENT_METRICS_PING_FRESH;
    }
    if (net_latency_stats_fresh(&g.net_ack_latency,
                                g.net_time,
                                NET_LATENCY_STALE_SEC)) {
        flags |= NET_CLIENT_METRICS_ACK_FRESH;
    }
    if (g.net_ping_miss_windows_reported > 0)
        flags |= NET_CLIENT_METRICS_PING_MISSED;
    if (g.net_ack_miss_windows_reported > 0)
        flags |= NET_CLIENT_METRICS_ACK_MISSED;
    return flags;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_smoothed_ack_gap_ms(void) {
    if (!net_latency_gap_stats_fresh(&g.net_ack_gap,
                                     g.net_time,
                                     NET_LATENCY_STALE_SEC)) {
        return 0.0f;
    }
    return net_latency_gap_stats_smoothed_sec(&g.net_ack_gap) * 1000.0f;
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
float get_net_motion_raw_player_interval_ms(void) {
    return g.net_motion.raw_packet_interval * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_raw_player_interval_ms(void) {
    return g.net_motion.max_raw_packet_interval_run * 1000.0f;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
float get_net_motion_max_raw_player_jitter_ms(void) {
    return g.net_motion.max_raw_packet_jitter_run * 1000.0f;
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
int get_net_motion_input_apply_error_ticks(void) {
    return (int)g.net_motion.input_apply_error_ticks;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_max_input_apply_error_abs(void) {
    return (int)g.net_motion.max_input_apply_error_abs;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_input_lead_margin_ticks(void) {
    return (int)g.net_motion.input_lead_margin_ticks;
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
    return (int)net_unacked_input_count();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_missed_pongs(void) {
    return (int)g.net_missed_pongs;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_missed_input_acks(void) {
    return (int)g.net_missed_input_acks;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_ack_recovery_packets(void) {
    return (int)g.net_ack_recovery_packets;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int get_net_motion_ack_recovery_tier(void) {
    return (int)g.net_ack_recovery_tier;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
int signal_smoke_prepare_known_ledger_sync(void) {
    if (!g.local_server.active || !net_is_loopback()) return 0;
    int player_idx = g.local_player_slot;
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return 0;

    world_t *authority = local_server_world(&g.local_server);
    if (!authority) return 0;
    server_player_t *server_player = &authority->players[player_idx];
    server_player_t *client_player = &g.world.players[player_idx];
    const int earned_station = 0;
    const int current_station = 2;
    if (!server_player->connected || !server_player->ship ||
        !server_player->replication ||
        !client_player->connected || !client_player->ship ||
        !station_exists(&authority->stations[earned_station]) ||
        !station_exists(&authority->stations[current_station])) {
        return 0;
    }

    /* Seed only the recipient's Prospect balance. Helios is intentionally
     * zero and every other station is absent, so the packet proves that
     * unknown is not silently serialized as authoritative zero. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        authority->stations[s].ledger_count = 0;
        g.world.stations[s].ledger_count = 0;
    }
    if (server_player_can_use_pubkey_persistence(server_player)) {
        ledger_earn_by_pubkey(&authority->stations[earned_station],
                              server_player->pubkey, 123.0f);
    } else {
        ledger_earn(&authority->stations[earned_station],
                    server_player->session_token, 123.0f);
    }

    server_player->session_ready = true;
    server_player->docked = true;
    server_player->docking_approach = false;
    server_player->current_station = current_station;
    server_player->nearby_station = current_station;
    server_player->in_dock_range = true;
    server_player->ship->pos = authority->stations[current_station].pos;
    server_player->ship->vel = v2(0.0f, 0.0f);

    client_player->docked = false;
    client_player->current_station = -1;
    client_player->nearby_station = -1;
    client_player->in_dock_range = false;
    g.known_station_ledger_count = 0;
    memset(g.known_station_ledger, 0, sizeof(g.known_station_ledger));
    g.station_balance = -999.0f;
    g.action_predict_timer = 0.0f;
    g.local_credit_hint_shown = false;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    g.local_server.private_snapshot_dirty = true;
    server_player->replication->force_authoritative_resync = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_known_ledger_sync_state(void) {
    if (!g.local_server.active || !net_is_loopback()) return 0;
    int state = 0;
    bool prospect_balance = false;
    bool unexpected_station = false;
    if (g.known_station_ledger_count == 1) state |= 1 << 0;
    for (int i = 0; i < g.known_station_ledger_count; i++) {
        const NetKnownLedgerEntry *entry = &g.known_station_ledger[i];
        if (entry->station == 0 && fabsf(entry->balance - 123.0f) < 0.01f)
            prospect_balance = true;
        else
            unexpected_station = true;
    }
    if (prospect_balance) state |= 1 << 1;
    if (!unexpected_station) state |= 1 << 2;
    if (LOCAL_PLAYER.docked && LOCAL_PLAYER.current_station == 2)
        state |= 1 << 3;
    if (fabsf(g.station_balance) < 0.01f) state |= 1 << 4;

    char summary[256];
    if (station_credit_perception_summary(summary, sizeof(summary))) {
        if (strstr(summary, "Prospect")) state |= 1 << 5;
        if (strstr(summary, "Helios")) state |= 1 << 6;
        if (strstr(summary, "buy > haul")) state |= 1 << 7;
    }
    if (prospect_balance && LOCAL_PLAYER.docked &&
        LOCAL_PLAYER.current_station == 2 &&
        fabsf(g.station_balance) < 0.01f) {
        maybe_notice_local_credits_rule(2, g.station_balance);
    }
    if (g.local_credit_hint_shown && strstr(g.notice, "Credits stay local") &&
        strstr(g.notice, "Prospect")) {
        state |= 1 << 8;
    }
    return state;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_prepare_tow_lifecycle(void) {
    if (!g.local_server.active || !net_is_loopback()) return 0;
    int player_idx = g.local_player_slot;
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return 0;

    world_t *authority = local_server_world(&g.local_server);
    if (!authority) return 0;
    server_player_t *server_player = &authority->players[player_idx];
    server_player_t *client_player = &g.world.players[player_idx];
    if (!server_player->connected || !server_player->ship ||
        !client_player->connected || !client_player->ship) {
        return 0;
    }

    const int target_idx = MAX_ASTEROIDS - 1;
    const int pod_idx = MAX_CARGO_PODS - 1;
    const vec2 test_pos = {
        server_player->ship->pos.x + 600.0f,
        server_player->ship->pos.y,
    };

    world_tow_links_clear_source(authority, server_player->ship_ref);
    world_asteroid_clear_tractor(authority, target_idx);
    memset(&authority->asteroids[target_idx], 0,
           sizeof(authority->asteroids[target_idx]));
    authority->asteroid_generation_live[target_idx] = false;
    authority->asteroids[target_idx] = (asteroid_t){
        .active = true,
        .fracture_child = true,
        .tier = ASTEROID_TIER_S,
        .commodity = COMMODITY_FERRITE_ORE,
        .pos = {test_pos.x + 90.0f, test_pos.y},
        .vel = {0.0f, 0.0f},
        .hp = 8.0f,
        .max_hp = 8.0f,
        .ore = 3.0f,
        .radius = 14.0f,
        .grade = MINING_GRADE_COMMON,
        .net_dirty = true,
    };
    world_cargo_pod_clear_tractor(authority, pod_idx);
    memset(&authority->cargo_pods[pod_idx], 0,
           sizeof(authority->cargo_pods[pod_idx]));
    authority->cargo_pod_generation_live[pod_idx] = false;
    authority->cargo_pods[pod_idx] = (cargo_pod_t){
        .active = true,
        .kind = CARGO_POD_CARGO,
        .commodity = COMMODITY_FERRITE_INGOT,
        .pos = {test_pos.x - 90.0f, test_pos.y},
        .vel = {0.0f, 0.0f},
        .radius = 18.0f,
        .quantity = 12,
    };

    server_player->session_ready = true;
    server_player->docked = false;
    server_player->in_dock_range = false;
    server_player->nearby_station = -1;
    server_player->ship->pos = test_pos;
    server_player->ship->vel = v2(0.0f, 0.0f);
    server_player->ship->tractor_active = false;
    memset(&server_player->input, 0, sizeof(server_player->input));
    server_player->movement_queue_count = 0;
    server_player->replication->force_authoritative_resync = true;
    g.local_server.private_snapshot_dirty = true;

    client_player->docked = false;
    client_player->in_dock_range = false;
    client_player->nearby_station = -1;
    client_player->ship->pos = test_pos;
    client_player->ship->vel = v2(0.0f, 0.0f);
    client_player->ship->tractor_active = false;
    client_player->ship->towed_count = 0;
    client_player->ship->towed_pod_count = 0;
    memset(client_player->ship->towed_fragments, -1,
           sizeof(client_player->ship->towed_fragments));
    memset(client_player->ship->towed_pods, -1,
           sizeof(client_player->ship->towed_pods));
    memset(&g.world.asteroids[target_idx], 0,
           sizeof(g.world.asteroids[target_idx]));
    memset(&g.asteroid_interp.prev[target_idx], 0,
           sizeof(g.asteroid_interp.prev[target_idx]));
    memset(&g.asteroid_interp.curr[target_idx], 0,
           sizeof(g.asteroid_interp.curr[target_idx]));
    g.asteroid_interp.elapsed[target_idx] = 0.0f;
    memset(&g.world.cargo_pods[pod_idx], 0,
           sizeof(g.world.cargo_pods[pod_idx]));
    memset(&g.cargo_pod_interp.prev[pod_idx], 0,
           sizeof(g.cargo_pod_interp.prev[pod_idx]));
    memset(&g.cargo_pod_interp.curr[pod_idx], 0,
           sizeof(g.cargo_pod_interp.curr[pod_idx]));
    g.cargo_pod_interp.elapsed[pod_idx] = 0.0f;
    g.plan_mode_active = false;
    g.death_cinematic.active = false;
    g.input.tractor_press_time = 0.0f;
    g.input.tractor_press_active = false;
    g.input.tractor_release_tap_pending = false;
    g.net_input_have_last = false;
    return target_idx + 1;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_tow_lifecycle_state(void) {
    if (!g.local_server.active || !net_is_loopback()) return 0;
    int player_idx = g.local_player_slot;
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return 0;
    const int target_idx = MAX_ASTEROIDS - 1;
    const int pod_idx = MAX_CARGO_PODS - 1;
    const world_t *authority = local_server_world_const(&g.local_server);
    if (!authority) return 0;
    const server_player_t *server_player = &authority->players[player_idx];
    const server_player_t *client_player = &g.world.players[player_idx];
    int state = 0;
    if (authority->asteroids[target_idx].active) state |= 1 << 0;
    if (g.world.asteroids[target_idx].active) state |= 1 << 1;
    if (server_player->ship && server_player->ship->tractor_active)
        state |= 1 << 2;
    if (client_player->ship && client_player->ship->tractor_active)
        state |= 1 << 3;
    if (server_player->ship) {
        for (int t = 0; t < server_player->ship->towed_count; t++) {
            if (server_player->ship->towed_fragments[t] == target_idx) {
                state |= 1 << 4;
                break;
            }
        }
    }
    if (client_player->ship) {
        for (int t = 0; t < client_player->ship->towed_count; t++) {
            if (client_player->ship->towed_fragments[t] == target_idx) {
                state |= 1 << 5;
                break;
            }
        }
    }
    if (asteroid_tractor_player(&authority->asteroids[target_idx]) ==
        player_idx) {
        state |= 1 << 6;
    }
    if (g.input.tractor_press_active) state |= 1 << 7;
    if (authority->cargo_pods[pod_idx].active) state |= 1 << 8;
    if (g.world.cargo_pods[pod_idx].active) state |= 1 << 9;
    if (server_player->ship) {
        for (int t = 0; t < server_player->ship->towed_pod_count; t++) {
            if (server_player->ship->towed_pods[t] == pod_idx) {
                state |= 1 << 10;
                break;
            }
        }
    }
    if (client_player->ship) {
        for (int t = 0; t < client_player->ship->towed_pod_count; t++) {
            if (client_player->ship->towed_pods[t] == pod_idx) {
                state |= 1 << 11;
                break;
            }
        }
    }
    if (cargo_pod_player_tractor(&authority->cargo_pods[pod_idx]) ==
        player_idx) {
        state |= 1 << 12;
    }
    if (cargo_pod_player_tractor(&g.cargo_pod_interp.curr[pod_idx]) ==
        player_idx) {
        state |= 1 << 13;
    }
    if (cargo_pod_player_tractor(&g.world.cargo_pods[pod_idx]) ==
        player_idx) {
        state |= 1 << 14;
    }
    if (g.tow_snapshot_received) state |= 1 << 15;
    if (g.tow_snapshot_received &&
        g.tow_snapshot_revision == authority->tow_revision &&
        g.tow_snapshot_server_tick == authority->tow_revision_tick) {
        state |= 1 << 16;
    }
    if (g.input.tractor_release_tap_pending) state |= 1 << 17;
    return state;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_remote_towable_interp_check(void) {
    world_t *local_authority = local_server_world(&g.local_server);
    if (!local_authority) return 0;
    bool saved_local_server_active = g.local_server.active;
    bool saved_net_authority_enabled = g.net_authority_enabled;
    bool saved_net_input_tick_protocol = g.net_input_tick_protocol;
    bool saved_tow_snapshot_received = g.tow_snapshot_received;
    uint32_t saved_tow_snapshot_revision = g.tow_snapshot_revision;
    uint32_t saved_tow_snapshot_server_tick = g.tow_snapshot_server_tick;
    uint32_t saved_world_tow_revision = g.world.tow_revision;
    uint32_t saved_world_tow_revision_tick = g.world.tow_revision_tick;
    tow_link_t saved_world_tow_links[MAX_TOW_LINKS];
    int saved_local_player_slot = g.local_player_slot;
    bool saved_player0_connected = g.world.players[0].connected;
    bool saved_player0_docked = g.world.players[0].docked;
    uint8_t saved_player0_towed_count = g.world.players[0].ship->towed_count;
    int16_t saved_player0_towed_fragments[10];
    uint8_t saved_player0_towed_pod_count =
        g.world.players[0].ship->towed_pod_count;
    int16_t saved_player0_towed_pods[10];
    asteroid_t saved_world_asteroids[MAX_ASTEROIDS];
    asteroid_t saved_local_server_asteroids[MAX_ASTEROIDS];
    asteroid_t saved_asteroid_prev[MAX_ASTEROIDS];
    asteroid_t saved_asteroid_curr[MAX_ASTEROIDS];
    float saved_asteroid_elapsed[MAX_ASTEROIDS];
    vec2 saved_asteroid_snapshot_vel[MAX_ASTEROIDS];
    vec2 saved_asteroid_acceleration[MAX_ASTEROIDS];
    float saved_asteroid_snapshot_elapsed[MAX_ASTEROIDS];
    bool saved_asteroid_snapshot_valid[MAX_ASTEROIDS];
    bool saved_asteroid_acceleration_valid[MAX_ASTEROIDS];
    client_npc_render_state_t saved_npc_prev[MAX_NPC_SHIPS];
    client_npc_render_state_t saved_npc_curr[MAX_NPC_SHIPS];
    float saved_npc_elapsed[MAX_NPC_SHIPS];
    sim_interactions_t saved_world_interactions = g.world.interactions;
    scaffold_t saved_world_scaffolds[MAX_SCAFFOLDS];
    scaffold_t saved_scaffold_prev[MAX_SCAFFOLDS];
    scaffold_t saved_scaffold_curr[MAX_SCAFFOLDS];
    float saved_scaffold_elapsed[MAX_SCAFFOLDS];
    cargo_pod_t saved_world_cargo_pods[MAX_CARGO_PODS];
    cargo_pod_t saved_cargo_pod_prev[MAX_CARGO_PODS];
    cargo_pod_t saved_cargo_pod_curr[MAX_CARGO_PODS];
    float saved_cargo_pod_elapsed[MAX_CARGO_PODS];

    memcpy(saved_player0_towed_fragments,
           g.world.players[0].ship->towed_fragments,
           sizeof(saved_player0_towed_fragments));
    memcpy(saved_player0_towed_pods,
           g.world.players[0].ship->towed_pods,
           sizeof(saved_player0_towed_pods));
    memcpy(saved_world_tow_links, g.world.tow_links,
           sizeof(saved_world_tow_links));
    memcpy(saved_world_asteroids, g.world.asteroids, sizeof(saved_world_asteroids));
    memcpy(saved_local_server_asteroids, local_authority->asteroids,
           sizeof(saved_local_server_asteroids));
    memcpy(saved_asteroid_prev, g.asteroid_interp.prev, sizeof(saved_asteroid_prev));
    memcpy(saved_asteroid_curr, g.asteroid_interp.curr, sizeof(saved_asteroid_curr));
    memcpy(saved_asteroid_elapsed, g.asteroid_interp.elapsed,
           sizeof(saved_asteroid_elapsed));
    memcpy(saved_asteroid_snapshot_vel, g.asteroid_interp.snapshot_vel,
           sizeof(saved_asteroid_snapshot_vel));
    memcpy(saved_asteroid_acceleration, g.asteroid_interp.acceleration,
           sizeof(saved_asteroid_acceleration));
    memcpy(saved_asteroid_snapshot_elapsed,
           g.asteroid_interp.snapshot_elapsed,
           sizeof(saved_asteroid_snapshot_elapsed));
    memcpy(saved_asteroid_snapshot_valid,
           g.asteroid_interp.snapshot_valid,
           sizeof(saved_asteroid_snapshot_valid));
    memcpy(saved_asteroid_acceleration_valid,
           g.asteroid_interp.acceleration_valid,
           sizeof(saved_asteroid_acceleration_valid));
    memcpy(saved_npc_prev, g.npc_interp.prev, sizeof(saved_npc_prev));
    memcpy(saved_npc_curr, g.npc_interp.curr, sizeof(saved_npc_curr));
    memcpy(saved_npc_elapsed, g.npc_interp.elapsed,
           sizeof(saved_npc_elapsed));
    memcpy(saved_world_scaffolds, g.world.scaffolds, sizeof(saved_world_scaffolds));
    memcpy(saved_scaffold_prev, g.scaffold_interp.prev, sizeof(saved_scaffold_prev));
    memcpy(saved_scaffold_curr, g.scaffold_interp.curr, sizeof(saved_scaffold_curr));
    memcpy(saved_scaffold_elapsed, g.scaffold_interp.elapsed,
           sizeof(saved_scaffold_elapsed));
    memcpy(saved_world_cargo_pods, g.world.cargo_pods, sizeof(saved_world_cargo_pods));
    memcpy(saved_cargo_pod_prev, g.cargo_pod_interp.prev, sizeof(saved_cargo_pod_prev));
    memcpy(saved_cargo_pod_curr, g.cargo_pod_interp.curr, sizeof(saved_cargo_pod_curr));
    memcpy(saved_cargo_pod_elapsed, g.cargo_pod_interp.elapsed,
           sizeof(saved_cargo_pod_elapsed));

    g.local_server.active = false;
    /* Exercise legacy compatibility packets first. Once an atomic snapshot
     * arrives below, those split fields must stop mutating tow authority. */
    g.tow_snapshot_received = false;
    g.tow_snapshot_revision = 0;
    g.tow_snapshot_server_tick = 0;
    memset(g.world.tow_links, 0, sizeof(g.world.tow_links));
    g.world.tow_revision = 0;
    g.world.tow_revision_tick = 0;
    g.world.players[0].ship->towed_count = 0;
    g.world.players[0].ship->towed_pod_count = 0;
    for (int i = 0; i < 10; i++) {
        g.world.players[0].ship->towed_pods[i] = -1;
    }
    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    memset(&g.world.interactions, 0, sizeof(g.world.interactions));
    memset(g.world.scaffolds, 0, sizeof(g.world.scaffolds));
    memset(&g.scaffold_interp, 0, sizeof(g.scaffold_interp));
    memset(g.world.cargo_pods, 0, sizeof(g.world.cargo_pods));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));

    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    NetNpcState npc_pair[2] = {
        {
            .index = 3,
            .flags = 1u,
            .x = 0.0f,
            .y = 0.0f,
            .vx = 100.0f,
            .vy = 0.0f,
            .target_asteroid = -1,
            .towed_fragment = -1,
            .home_station = 0xFFu,
        },
        {
            .index = 4,
            .flags = 1u,
            .x = 0.0f,
            .y = 90.0f,
            .vx = 0.0f,
            .vy = 0.0f,
            .target_asteroid = -1,
            .towed_fragment = -1,
            .home_station = 0xFFu,
        },
    };
    apply_remote_npcs(npc_pair, 2);
    g.npc_interp.elapsed[3] = 0.1f;
    vec2 npc_before_unrelated_pos = {0};
    vec2 npc_before_unrelated_vel = {0};
    bool npc_before_unrelated_visible = net_remote_npc_presentation(
        3, &npc_before_unrelated_pos, &npc_before_unrelated_vel);
    NetNpcPosState unrelated_npc_pos = {
        .index = 4,
        .x = 5.0f,
        .y = 90.0f,
    };
    apply_remote_npc_pos(&unrelated_npc_pos, 1);
    net_advance_npc_interpolation(0.05f);
    vec2 npc_after_unrelated_pos = {0};
    vec2 npc_after_unrelated_vel = {0};
    bool npc_after_unrelated_visible = net_remote_npc_presentation(
        3, &npc_after_unrelated_pos, &npc_after_unrelated_vel);

    NetScaffoldState scaffold = {
        .index = 3,
        .state = SCAFFOLD_NASCENT,
        .module_type = MODULE_DOCK,
        .owner = -1,
        .pos_x = 0.0f,
        .pos_y = 0.0f,
        .vel_x = 100.0f,
        .vel_y = 0.0f,
        .radius = 30.0f,
        .build_amount = 0.0f,
        .built_at_station = 2,
    };
    apply_remote_scaffolds(&scaffold, 1);
    bool scaffold_station_authoritative =
        g.scaffold_interp.curr[3].built_at_station == 2;
    g.scaffold_interp.elapsed[3] = 0.1f;
    interpolate_world_for_render();
    float scaffold_first_x = g.world.scaffolds[3].pos.x;
    scaffold.pos_x = 100.0f;
    scaffold.vel_x = 0.0f;
    apply_remote_scaffolds(&scaffold, 1);
    g.scaffold_interp.elapsed[3] = 0.05f;
    interpolate_world_for_render();
    float scaffold_blended_x = g.world.scaffolds[3].pos.x;

    memset(g.world.scaffolds, 0, sizeof(g.world.scaffolds));
    memset(&g.scaffold_interp, 0, sizeof(g.scaffold_interp));
    scaffold.pos_x = 0.0f;
    scaffold.pos_y = 0.0f;
    scaffold.vel_x = 100.0f;
    NetScaffoldState unrelated_scaffold = scaffold;
    unrelated_scaffold.index = 4;
    unrelated_scaffold.pos_y = 90.0f;
    unrelated_scaffold.vel_x = 0.0f;
    NetScaffoldState scaffold_pair[2] = {scaffold, unrelated_scaffold};
    apply_remote_scaffolds(scaffold_pair, 2);
    g.scaffold_interp.elapsed[3] = 0.1f;
    interpolate_world_for_render();
    float scaffold_before_unrelated_x = g.world.scaffolds[3].pos.x;
    NetScaffoldMotionState unrelated_scaffold_motion = {
        .index = 4,
        .pos_x = 5.0f,
        .pos_y = 90.0f,
        .vel_x = 0.0f,
        .vel_y = 0.0f,
    };
    apply_remote_scaffold_motion(&unrelated_scaffold_motion, 1);
    net_advance_scaffold_interpolation(0.05f);
    interpolate_world_for_render();
    float scaffold_after_unrelated_x = g.world.scaffolds[3].pos.x;

    NetCargoPodState pod = {
        .index = 5,
        .kind = CARGO_POD_CARGO,
        .commodity = COMMODITY_FERRITE_INGOT,
        .tractor_player = -1,
        .pos_x = 0.0f,
        .pos_y = 0.0f,
        .vel_x = 100.0f,
        .vel_y = 0.0f,
        .radius = 18.0f,
        .rotation = 0.0f,
        .quantity = 12,
    };
    apply_remote_cargo_pods(&pod, 1);
    g.cargo_pod_interp.elapsed[5] = 0.1f;
    interpolate_world_for_render();
    float pod_first_x = g.world.cargo_pods[5].pos.x;
    pod.pos_x = 100.0f;
    pod.vel_x = 0.0f;
    apply_remote_cargo_pods(&pod, 1);
    interpolate_world_for_render();
    float pod_correction_start_vx = g.world.cargo_pods[5].vel.x;
    g.cargo_pod_interp.elapsed[5] = 0.05f;
    interpolate_world_for_render();
    float pod_blended_x = g.world.cargo_pods[5].pos.x;

    memset(g.world.cargo_pods, 0, sizeof(g.world.cargo_pods));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));
    pod.pos_x = 0.0f;
    pod.pos_y = 0.0f;
    pod.vel_x = 100.0f;
    NetCargoPodState unrelated_pod = pod;
    unrelated_pod.index = 6;
    unrelated_pod.pos_y = 90.0f;
    unrelated_pod.vel_x = 0.0f;
    NetCargoPodState pod_pair[2] = {pod, unrelated_pod};
    apply_remote_cargo_pods(pod_pair, 2);
    g.cargo_pod_interp.elapsed[5] = 0.1f;
    interpolate_world_for_render();
    float pod_before_unrelated_x = g.world.cargo_pods[5].pos.x;
    NetCargoPodMotionState unrelated_pod_motion = {
        .index = 6,
        .pos_x = 5.0f,
        .pos_y = 90.0f,
        .vel_x = 0.0f,
        .vel_y = 0.0f,
        .rotation = 0.0f,
    };
    apply_remote_cargo_pod_motion(&unrelated_pod_motion, 1);
    net_advance_cargo_pod_interpolation(0.05f);
    interpolate_world_for_render();
    float pod_after_unrelated_x = g.world.cargo_pods[5].pos.x;

    g.net_authority_enabled = true;
    g.local_player_slot = 0;
    g.world.players[0].connected = true;
    g.world.players[0].ship->towed_pod_count = 0;
    for (int i = 0; i < 10; i++) {
        g.world.players[0].ship->towed_pods[i] = -1;
    }
    NetCargoPodState held_pod = pod;
    held_pod.index = 11;
    held_pod.tractor_player = 0;
    apply_remote_cargo_pods(&held_pod, 1);
    bool pod_roster_attach_ok =
        g.world.players[0].ship->towed_pod_count == 1 &&
        g.world.players[0].ship->towed_pods[0] == 11;
    held_pod.tractor_player = -1;
    apply_remote_cargo_pods(&held_pod, 1);
    bool pod_roster_detach_ok =
        g.world.players[0].ship->towed_pod_count == 0;

    /* Local tow prediction has already advanced the simulated pod to this
     * pose. Adopting it into the render stream and then advancing the render
     * clock once must reconstruct that same pose, not extrapolate by dt a
     * second time. */
    memset(g.world.cargo_pods, 0, sizeof(g.world.cargo_pods));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));
    g.net_input_tick_protocol = true;
    g.world.players[0].docked = false;
    held_pod.tractor_player = 0;
    held_pod.pos_x = 200.0f;
    held_pod.vel_x = 100.0f;
    apply_remote_cargo_pods(&held_pod, 1);
    g.world.cargo_pods[11] = g.cargo_pod_interp.curr[11];
    g.world.cargo_pods[11].pos.x = 300.0f;
    cargo_pod_clear_tractor(&g.world.cargo_pods[11]);
    g.cargo_pod_interp.elapsed[11] = 0.10f;
    net_adopt_local_tow_prediction(0.05f);
    bool local_towed_pod_binding_preserved =
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[11]) == 0;
    net_advance_cargo_pod_interpolation(0.05f);
    interpolate_world_for_render();
    float local_towed_pod_predicted_x = g.world.cargo_pods[11].pos.x;

    tow_link_t atomic_link = {
        .active = true,
        .source = g.world.players[0].ship_ref,
        .target = {
            .kind = ENTITY_KIND_CARGO_POD,
            .index = 11,
            .part = -1,
            .generation = 1,
        },
        .profile = TOW_PROFILE_SHIP_POD,
        .slot = 0,
        .state = TOW_LINK_HELD,
        .attached_tick = 400,
        .revision = 40,
    };
    /* Online clients allocate component generations independently from the
     * server. The authenticated relation generation must still project onto
     * the live player slot, or local tractor lines disappear after restart or
     * reconnect. */
    atomic_link.source.generation += 7;
    if (atomic_link.source.generation == 0)
        atomic_link.source.generation = 1;
    apply_remote_tow_links(&atomic_link, 1, 40, 400);
    bool atomic_attach_ok =
        g.tow_snapshot_received &&
        g.tow_snapshot_revision == 40 &&
        g.tow_snapshot_server_tick == 400 &&
        g.world.tow_links[0].active &&
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[11]) == 0;
    apply_remote_tow_links(NULL, 0, 39, 401);
    bool stale_release_rejected =
        g.world.tow_links[0].active &&
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[11]) == 0;
    apply_remote_tow_links(NULL, 0, 40, 402);
    bool conflicting_duplicate_idempotent =
        g.world.tow_links[0].active &&
        g.tow_snapshot_server_tick == 400 &&
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[11]) == 0;
    tow_link_t conflicting_links[2] = { atomic_link, atomic_link };
    conflicting_links[0].revision = 41;
    conflicting_links[1].revision = 41;
    conflicting_links[1].target.index = 12;
    apply_remote_tow_links(conflicting_links, 2, 41, 403);
    bool malformed_replacement_rejected =
        g.tow_snapshot_revision == 40 &&
        g.world.tow_links[0].active &&
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[11]) == 0;
    apply_remote_tow_links(NULL, 0, 41, 404);
    bool newer_release_applied =
        !g.world.tow_links[0].active &&
        g.tow_snapshot_revision == 41 &&
        g.tow_snapshot_server_tick == 404 &&
        !cargo_pod_has_player_tractor(&g.cargo_pod_interp.curr[11]);

    /*
     * A dock/furnace module may hold multiple physical pods. MODULE_POD
     * compatibility slots are all zero, so distinct targets sharing the
     * same source/profile/slot must survive atomic validation together.
     */
    for (int pod_idx = 12; pod_idx <= 13; pod_idx++) {
        g.world.cargo_pods[pod_idx].active = true;
        g.cargo_pod_interp.prev[pod_idx].active = true;
        g.cargo_pod_interp.curr[pod_idx].active = true;
    }
    tow_link_t module_links[2] = {
        {
            .active = true,
            .source = {
                .kind = ENTITY_KIND_STATION_MODULE,
                .index = 0,
                .part = 0,
                .generation = 1,
            },
            .target = {
                .kind = ENTITY_KIND_CARGO_POD,
                .index = 12,
                .part = -1,
                .generation = 1,
            },
            .profile = TOW_PROFILE_MODULE_POD,
            .slot = 0,
            .state = TOW_LINK_HELD,
            .attached_tick = 405,
            .revision = 42,
        },
        {
            .active = true,
            .source = {
                .kind = ENTITY_KIND_STATION_MODULE,
                .index = 0,
                .part = 0,
                .generation = 1,
            },
            .target = {
                .kind = ENTITY_KIND_CARGO_POD,
                .index = 13,
                .part = -1,
                .generation = 2,
            },
            .profile = TOW_PROFILE_MODULE_POD,
            .slot = 0,
            .state = TOW_LINK_HELD,
            .attached_tick = 405,
            .revision = 42,
        },
    };
    apply_remote_tow_links(module_links, 2, 42, 405);
    bool multi_pod_module_snapshot_ok =
        g.tow_snapshot_revision == 42 &&
        g.world.tow_links[0].active &&
        g.world.tow_links[1].active &&
        cargo_pod_is_tractored_by_module(
            &g.cargo_pod_interp.curr[12], 0, 0) &&
        cargo_pod_is_tractored_by_module(
            &g.cargo_pod_interp.curr[13], 0, 0);

    /* A current relation stays stored while its source is absent, then an
     * equal-revision delivery may re-project it when that roster slot is live
     * again. This keeps the stale-actor guard without comparing unrelated
     * client/server component generations. */
    tow_link_t roster_ship_source = atomic_link;
    roster_ship_source.revision = 43;
    g.world.players[0].connected = false;
    apply_remote_tow_links(&roster_ship_source, 1, 43, 406);
    bool absent_ship_source_not_projected =
        g.tow_snapshot_revision == 43 &&
        !cargo_pod_has_player_tractor(
            &g.cargo_pod_interp.curr[roster_ship_source.target.index]);
    g.world.players[0].connected = true;
    apply_remote_tow_links(&roster_ship_source, 1, 43, 407);
    bool live_ship_source_reprojected =
        cargo_pod_player_tractor(
            &g.cargo_pod_interp.curr[roster_ship_source.target.index]) == 0;

    tow_link_t relevance_link = atomic_link;
    relevance_link.target.index = 12;
    relevance_link.target.generation = 3;
    relevance_link.revision = 44;
    apply_remote_tow_links(&relevance_link, 1, 44, 407);
    bool relevant_target_projected =
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[12]) == 0;
    g.cargo_pod_interp.curr[12].active = false;
    apply_remote_tow_links(&relevance_link, 1, 44, 408);
    bool irrelevant_target_not_projected =
        !cargo_pod_has_player_tractor(&g.cargo_pod_interp.curr[12]) &&
        g.tow_snapshot_server_tick == 407;
    g.cargo_pod_interp.curr[12].active = true;
    apply_remote_tow_links(&relevance_link, 1, 44, 409);
    bool relevance_reentry_reprojected =
        cargo_pod_player_tractor(&g.cargo_pod_interp.curr[12]) == 0 &&
        g.tow_snapshot_server_tick == 407;

    apply_remote_tow_links(NULL, 0, 45, 410);
    apply_remote_tow_links(&relevance_link, 1, 44, 411);
    bool recycled_target_rejects_stale_relation =
        g.tow_snapshot_revision == 45 &&
        !cargo_pod_has_player_tractor(&g.cargo_pod_interp.curr[12]);
    bool atomic_tow_snapshot_ok =
        atomic_attach_ok && stale_release_rejected &&
        conflicting_duplicate_idempotent &&
        malformed_replacement_rejected && newer_release_applied &&
        multi_pod_module_snapshot_ok &&
        absent_ship_source_not_projected &&
        live_ship_source_reprojected &&
        relevant_target_projected &&
        irrelevant_target_not_projected &&
        relevance_reentry_reprojected &&
        recycled_target_rejects_stale_relation;

    /*
     * The remaining fixture cases exercise the legacy split tow projection
     * path directly. End the atomic-snapshot subcase so roster relevance
     * transitions do not correctly rebuild from its final empty relation
     * set and erase those intentionally hand-seeded compatibility fields.
     */
    g.tow_snapshot_received = false;
    g.tow_snapshot_revision = 0;
    g.tow_snapshot_server_tick = 0;

    NetAsteroidState asteroid = {
        .index = 7,
        .flags = (uint8_t)(1u | (1u << 1) |
                 (((uint8_t)ASTEROID_TIER_S & 0x7u) << 2) |
                 (((uint8_t)COMMODITY_FERRITE_ORE & 0x7u) << 5)),
        .x = 0.0f,
        .y = 0.0f,
        .vx = 100.0f,
        .vy = 0.0f,
        .hp = 10.0f,
        .ore = 4.0f,
        .radius = 18.0f,
        .smelt_progress = 0.0f,
        .grade = MINING_GRADE_COMMON,
        .crystal_stage = 0,
        .phase = 0,
    };
    apply_remote_asteroids(&asteroid, 1);
    g.asteroid_interp.elapsed[7] = 0.1f;
    interpolate_world_for_render();
    float asteroid_first_x = g.world.asteroids[7].pos.x;
    asteroid.x = 100.0f;
    asteroid.vx = 0.0f;
    apply_remote_asteroids(&asteroid, 1);
    g.asteroid_interp.elapsed[7] = 0.05f;
    interpolate_world_for_render();
    float asteroid_blended_x = g.world.asteroids[7].pos.x;

    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    asteroid.index = 7;
    asteroid.x = 0.0f;
    asteroid.y = 0.0f;
    asteroid.vx = 100.0f;
    asteroid.vy = 0.0f;
    apply_remote_asteroids(&asteroid, 1);
    g.asteroid_interp.elapsed[7] = 0.1f;
    interpolate_world_for_render();
    float loose_asteroid_before_unrelated_x = g.world.asteroids[7].pos.x;
    NetAsteroidState unrelated_loose_asteroid = asteroid;
    unrelated_loose_asteroid.index = 8;
    unrelated_loose_asteroid.x = 0.0f;
    unrelated_loose_asteroid.y = 90.0f;
    unrelated_loose_asteroid.vx = 0.0f;
    unrelated_loose_asteroid.vy = 0.0f;
    apply_remote_asteroids(&unrelated_loose_asteroid, 1);
    net_advance_asteroid_interpolation(0.05f);
    interpolate_world_for_render();
    float loose_asteroid_after_unrelated_x = g.world.asteroids[7].pos.x;
    NetAsteroidMotionState unrelated_loose_motion = {
        .index = 8,
        .x = 30.0f,
        .y = 90.0f,
        .vx = 0.0f,
        .vy = 0.0f,
    };
    apply_remote_asteroid_motion(&unrelated_loose_motion, 1);
    net_advance_asteroid_interpolation(0.05f);
    interpolate_world_for_render();
    float loose_asteroid_after_unrelated_motion_x =
        g.world.asteroids[7].pos.x;

    /* A correction for slot A followed by a separate same-tick packet for
     * slot B must leave A's reconciliation intact. */
    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    asteroid_t correction_base = {0};
    correction_base.active = true;
    correction_base.fracture_child = true;
    correction_base.tier = ASTEROID_TIER_S;
    correction_base.commodity = COMMODITY_FERRITE_ORE;
    correction_base.radius = 18.0f;
    correction_base.hp = 10.0f;
    correction_base.ore = 4.0f;
    g.asteroid_interp.prev[7] = correction_base;
    g.asteroid_interp.curr[7] = correction_base;
    correction_base.pos.y = 90.0f;
    g.asteroid_interp.prev[8] = correction_base;
    g.asteroid_interp.curr[8] = correction_base;
    NetAsteroidMotionState correction_a = {
        .index = 7,
        .x = 100.0f,
        .y = 0.0f,
        .vx = 0.0f,
        .vy = 0.0f,
    };
    NetAsteroidMotionState correction_b = {
        .index = 8,
        .x = 30.0f,
        .y = 90.0f,
        .vx = 0.0f,
        .vy = 0.0f,
    };
    apply_remote_asteroid_motion(&correction_a, 1);
    apply_remote_asteroid_motion(&correction_b, 1);
    g.asteroid_interp.elapsed[7] = 0.18f;

    /* Free-motion prediction must continue across a multi-second quiet
     * cadence and follow the server's ambient-drag curve. */
    asteroid_t drag_baseline = correction_base;
    drag_baseline.pos = v2(0.0f, 180.0f);
    drag_baseline.vel = v2(20.0f, 0.0f);
    g.asteroid_interp.prev[9].active = false;
    g.asteroid_interp.curr[9] = drag_baseline;
    g.asteroid_interp.elapsed[9] = 2.0f;
    interpolate_world_for_render();
    float same_tick_correction_x = g.world.asteroids[7].pos.x;
    float long_gap_drag_x = g.world.asteroids[9].pos.x;
    float long_gap_drag_vx = g.world.asteroids[9].vel.x;
    net_advance_asteroid_interpolation(0.55f);
    bool settled_correction_retired =
        !g.asteroid_interp.prev[7].active;

    /* Station tractors are authoritative interaction sources rather than
     * asteroid ownership bindings. Their targets must still use the
     * constant-velocity tow predictor between frequent motion updates. */
    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    g.world.interactions.count = 1;
    g.world.interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR,
        .source = {
            .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
            .index = 0,
            .aux = 1,
        },
        .target = {
            .type = SIM_INTERACTION_ENTITY_ASTEROID,
            .index = 7,
            .aux = -1,
        },
    };
    asteroid_t station_pulled_asteroid = correction_base;
    station_pulled_asteroid.pos = v2(0.0f, 270.0f);
    station_pulled_asteroid.vel = v2(20.0f, 0.0f);
    g.asteroid_interp.curr[7] = station_pulled_asteroid;
    g.asteroid_interp.elapsed[7] = 2.0f;
    interpolate_world_for_render();
    float station_pulled_asteroid_x = g.world.asteroids[7].pos.x;
    float station_pulled_asteroid_vx = g.world.asteroids[7].vel.x;

    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    memset(&g.world.interactions, 0, sizeof(g.world.interactions));
    g.net_authority_enabled = true;
    g.net_input_tick_protocol = true;
    g.local_player_slot = 0;
    g.world.players[0].connected = true;
    g.world.players[0].docked = false;
    memset(g.world.players[0].ship->towed_fragments, -1,
           sizeof(g.world.players[0].ship->towed_fragments));
    g.world.players[0].ship->towed_fragments[0] = 7;
    g.world.players[0].ship->towed_count = 1;

    asteroid_t predicted_asteroid = {0};
    predicted_asteroid.active = true;
    predicted_asteroid.fracture_child = true;
    predicted_asteroid.tier = ASTEROID_TIER_S;
    predicted_asteroid.commodity = COMMODITY_FERRITE_ORE;
    predicted_asteroid.pos = v2(250.0f, 0.0f);
    predicted_asteroid.vel = v2(20.0f, 0.0f);
    predicted_asteroid.hp = 10.0f;
    predicted_asteroid.ore = 4.0f;
    predicted_asteroid.radius = 18.0f;
    predicted_asteroid.grade = MINING_GRADE_COMMON;
    g.world.asteroids[7] = predicted_asteroid;
    g.asteroid_interp.curr[7] = predicted_asteroid;
    g.asteroid_interp.prev[7] = predicted_asteroid;

    asteroid.x = -200.0f;
    asteroid.y = 0.0f;
    asteroid.vx = 0.0f;
    asteroid.vy = 0.0f;
    asteroid.hp = 7.0f;
    asteroid.ore = 3.0f;
    apply_remote_asteroids(&asteroid, 1);
    interpolate_world_for_render();
    float local_towed_asteroid_x = g.world.asteroids[7].pos.x;
    float local_towed_asteroid_vx = g.world.asteroids[7].vel.x;
    float local_towed_asteroid_hp = g.world.asteroids[7].hp;

    predicted_asteroid.pos = v2(300.0f, 0.0f);
    predicted_asteroid.vel = v2(20.0f, 0.0f);
    predicted_asteroid.hp = 7.0f;
    predicted_asteroid.ore = 3.0f;
    g.world.asteroids[7] = predicted_asteroid;
    g.asteroid_interp.prev[7] = predicted_asteroid;
    g.asteroid_interp.curr[7] = predicted_asteroid;
    g.asteroid_interp.curr[7].pos.x = 200.0f;
    g.asteroid_interp.elapsed[7] = 0.05f;
    net_adopt_local_tow_prediction(0.0f);

    NetAsteroidState unrelated_asteroid = asteroid;
    unrelated_asteroid.index = 8;
    unrelated_asteroid.x = 0.0f;
    unrelated_asteroid.y = 80.0f;
    unrelated_asteroid.vx = 0.0f;
    unrelated_asteroid.vy = 0.0f;
    apply_remote_asteroids(&unrelated_asteroid, 1);
    interpolate_world_for_render();
    float local_towed_asteroid_unrelated_x =
        g.world.asteroids[7].pos.x;

    predicted_asteroid.pos = v2(350.0f, 0.0f);
    g.world.asteroids[7] = predicted_asteroid;
    g.asteroid_interp.prev[7] = predicted_asteroid;
    g.asteroid_interp.curr[7] = predicted_asteroid;
    g.asteroid_interp.curr[7].pos.x = 240.0f;
    g.asteroid_interp.elapsed[7] = 0.05f;
    net_adopt_local_tow_prediction(0.0f);

    NetAsteroidMotionState unrelated_motion = {
        .index = 8,
        .x = 30.0f,
        .y = 80.0f,
        .vx = 0.0f,
        .vy = 0.0f,
    };
    apply_remote_asteroid_motion(&unrelated_motion, 1);
    interpolate_world_for_render();
    float local_towed_asteroid_unrelated_motion_x =
        g.world.asteroids[7].pos.x;

    bool loopback_packet_path_ok = true;
    if (net_is_loopback()) {
        g.local_server.active = true;
        g.net_input_tick_protocol = true;
        memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
        memset(local_authority->asteroids, 0,
               sizeof(local_authority->asteroids));
        memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));

        asteroid_t prev = {0};
        prev.active = true;
        prev.pos.x = 0.0f;
        prev.pos.y = 0.0f;
        prev.vel.x = 0.0f;
        prev.vel.y = 0.0f;
        prev.radius = 20.0f;
        prev.tier = ASTEROID_TIER_M;
        prev.commodity = COMMODITY_FERRITE_ORE;
        asteroid_t curr = prev;
        curr.pos.x = 100.0f;
        curr.pos.y = 25.0f;
        g.asteroid_interp.prev[7] = prev;
        g.asteroid_interp.curr[7] = prev;
        local_authority->asteroids[7] = curr;
        g.asteroid_interp.elapsed[7] = 0.05f;

        interpolate_world_for_render();
        loopback_packet_path_ok =
            fabsf(g.world.asteroids[7].pos.x) < 0.001f &&
            fabsf(g.world.asteroids[7].pos.y) < 0.001f &&
            fabsf(g.asteroid_interp.curr[7].pos.x) < 0.001f &&
            g.asteroid_interp.elapsed[7] > 0.0f;
    }

    bool loopback_prediction_ok =
        !net_is_loopback() || net_local_prediction_enabled();

    int ok = scaffold_first_x > 9.0f && scaffold_first_x < 11.5f &&
             npc_before_unrelated_visible &&
             npc_after_unrelated_visible &&
             npc_before_unrelated_pos.x > 9.0f &&
             npc_before_unrelated_pos.x < 11.5f &&
             npc_after_unrelated_pos.x > 14.0f &&
             npc_after_unrelated_pos.x < 16.0f &&
             npc_before_unrelated_vel.x > 99.9f &&
             npc_after_unrelated_vel.x > 99.9f &&
             scaffold_blended_x > scaffold_first_x &&
             scaffold_blended_x < 95.0f &&
             scaffold_station_authoritative &&
             scaffold_before_unrelated_x > 9.0f &&
             scaffold_before_unrelated_x < 11.5f &&
             scaffold_after_unrelated_x > 14.0f &&
             scaffold_after_unrelated_x < 16.0f &&
             pod_first_x > 9.0f && pod_first_x < 11.5f &&
             pod_correction_start_vx > 99.9f &&
             pod_correction_start_vx < 100.1f &&
             pod_blended_x > pod_first_x &&
             pod_blended_x < 95.0f &&
             pod_before_unrelated_x > 9.0f &&
             pod_before_unrelated_x < 11.5f &&
             pod_after_unrelated_x > 14.0f &&
             pod_after_unrelated_x < 16.0f &&
             pod_roster_attach_ok &&
             pod_roster_detach_ok &&
             local_towed_pod_binding_preserved &&
             local_towed_pod_predicted_x > 299.9f &&
             local_towed_pod_predicted_x < 300.1f &&
             atomic_tow_snapshot_ok &&
             asteroid_first_x > 9.0f && asteroid_first_x < 11.5f &&
             asteroid_blended_x > asteroid_first_x &&
             asteroid_blended_x < 95.0f &&
             loose_asteroid_before_unrelated_x > 9.0f &&
             loose_asteroid_before_unrelated_x < 11.5f &&
             loose_asteroid_after_unrelated_x > 14.0f &&
             loose_asteroid_after_unrelated_x < 16.0f &&
             loose_asteroid_after_unrelated_motion_x > 19.0f &&
             loose_asteroid_after_unrelated_motion_x < 21.0f &&
             same_tick_correction_x > 88.0f &&
             same_tick_correction_x < 94.0f &&
             long_gap_drag_x > 26.0f && long_gap_drag_x < 28.5f &&
             long_gap_drag_vx > 8.0f && long_gap_drag_vx < 9.5f &&
             settled_correction_retired &&
             station_pulled_asteroid_x > 39.9f &&
             station_pulled_asteroid_x < 40.1f &&
             station_pulled_asteroid_vx > 19.9f &&
             station_pulled_asteroid_vx < 20.1f &&
             local_towed_asteroid_x > 249.0f &&
             local_towed_asteroid_x < 251.0f &&
             local_towed_asteroid_vx > 19.0f &&
             local_towed_asteroid_vx < 21.0f &&
             fabsf(local_towed_asteroid_hp - 7.0f) < 0.001f &&
             local_towed_asteroid_unrelated_x > 299.0f &&
             local_towed_asteroid_unrelated_x < 301.0f &&
             local_towed_asteroid_unrelated_motion_x > 349.0f &&
             local_towed_asteroid_unrelated_motion_x < 351.0f &&
             loopback_packet_path_ok &&
             loopback_prediction_ok;

    memcpy(g.world.asteroids, saved_world_asteroids, sizeof(saved_world_asteroids));
    memcpy(local_authority->asteroids, saved_local_server_asteroids,
           sizeof(saved_local_server_asteroids));
    memcpy(g.asteroid_interp.prev, saved_asteroid_prev, sizeof(saved_asteroid_prev));
    memcpy(g.asteroid_interp.curr, saved_asteroid_curr, sizeof(saved_asteroid_curr));
    memcpy(g.asteroid_interp.elapsed, saved_asteroid_elapsed,
           sizeof(saved_asteroid_elapsed));
    memcpy(g.asteroid_interp.snapshot_vel, saved_asteroid_snapshot_vel,
           sizeof(saved_asteroid_snapshot_vel));
    memcpy(g.asteroid_interp.acceleration, saved_asteroid_acceleration,
           sizeof(saved_asteroid_acceleration));
    memcpy(g.asteroid_interp.snapshot_elapsed,
           saved_asteroid_snapshot_elapsed,
           sizeof(saved_asteroid_snapshot_elapsed));
    memcpy(g.asteroid_interp.snapshot_valid,
           saved_asteroid_snapshot_valid,
           sizeof(saved_asteroid_snapshot_valid));
    memcpy(g.asteroid_interp.acceleration_valid,
           saved_asteroid_acceleration_valid,
           sizeof(saved_asteroid_acceleration_valid));
    memcpy(g.npc_interp.prev, saved_npc_prev, sizeof(saved_npc_prev));
    memcpy(g.npc_interp.curr, saved_npc_curr, sizeof(saved_npc_curr));
    memcpy(g.npc_interp.elapsed, saved_npc_elapsed,
           sizeof(saved_npc_elapsed));
    g.world.interactions = saved_world_interactions;
    memcpy(g.world.scaffolds, saved_world_scaffolds, sizeof(saved_world_scaffolds));
    memcpy(g.scaffold_interp.prev, saved_scaffold_prev, sizeof(saved_scaffold_prev));
    memcpy(g.scaffold_interp.curr, saved_scaffold_curr, sizeof(saved_scaffold_curr));
    memcpy(g.scaffold_interp.elapsed, saved_scaffold_elapsed,
           sizeof(saved_scaffold_elapsed));
    memcpy(g.world.cargo_pods, saved_world_cargo_pods, sizeof(saved_world_cargo_pods));
    memcpy(g.cargo_pod_interp.prev, saved_cargo_pod_prev, sizeof(saved_cargo_pod_prev));
    memcpy(g.cargo_pod_interp.curr, saved_cargo_pod_curr, sizeof(saved_cargo_pod_curr));
    memcpy(g.cargo_pod_interp.elapsed, saved_cargo_pod_elapsed,
           sizeof(saved_cargo_pod_elapsed));
    g.local_server.active = saved_local_server_active;
    g.net_authority_enabled = saved_net_authority_enabled;
    g.net_input_tick_protocol = saved_net_input_tick_protocol;
    g.tow_snapshot_received = saved_tow_snapshot_received;
    g.tow_snapshot_revision = saved_tow_snapshot_revision;
    g.tow_snapshot_server_tick = saved_tow_snapshot_server_tick;
    g.world.tow_revision = saved_world_tow_revision;
    g.world.tow_revision_tick = saved_world_tow_revision_tick;
    memcpy(g.world.tow_links, saved_world_tow_links,
           sizeof(saved_world_tow_links));
    g.local_player_slot = saved_local_player_slot;
    g.world.players[0].connected = saved_player0_connected;
    g.world.players[0].docked = saved_player0_docked;
    g.world.players[0].ship->towed_count = saved_player0_towed_count;
    memcpy(g.world.players[0].ship->towed_fragments,
           saved_player0_towed_fragments,
           sizeof(saved_player0_towed_fragments));
    g.world.players[0].ship->towed_pod_count = saved_player0_towed_pod_count;
    memcpy(g.world.players[0].ship->towed_pods,
           saved_player0_towed_pods,
           sizeof(saved_player0_towed_pods));
    return ok ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_local_tow_replay_stability_check(void) {
    return net_smoke_local_tow_replay_stability_check();
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_prepare_local_generation_mismatch_tether(void) {
    const int pod_idx = MAX_CARGO_PODS - 2;
    int player_idx = g.local_player_slot;
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return 0;
    server_player_t *player = &g.world.players[player_idx];
    if (!player->ship) return 0;

    /* Freeze the loopback authority so the render frame observes exactly the
     * online-style snapshot assembled below. */
    g.local_server.active = false;
    player->connected = true;
    player->docked = false;

    cargo_pod_t pod = {
        .active = true,
        .kind = CARGO_POD_CARGO,
        .commodity = COMMODITY_FERRITE_INGOT,
        .pos = {player->ship->pos.x + 100.0f, player->ship->pos.y},
        .vel = {0.0f, 0.0f},
        .radius = 18.0f,
        .quantity = 8,
    };
    g.world.cargo_pods[pod_idx] = pod;
    g.cargo_pod_interp.prev[pod_idx] = pod;
    g.cargo_pod_interp.curr[pod_idx] = pod;
    g.cargo_pod_interp.elapsed[pod_idx] = 0.0f;

    uint32_t revision = g.tow_snapshot_received
        ? g.tow_snapshot_revision + 1u : 1u;
    if (revision == 0) revision = 1;
    tow_link_t link = {
        .active = true,
        .source = player->ship_ref,
        .target = {
            .kind = ENTITY_KIND_CARGO_POD,
            .index = pod_idx,
            .part = -1,
            .generation = 1,
        },
        .profile = TOW_PROFILE_SHIP_POD,
        .slot = 0,
        .state = TOW_LINK_HELD,
        .attached_tick = g.world.tick,
        .revision = revision,
    };
    link.source.generation += 7;
    if (link.source.generation == 0) link.source.generation = 1;
    apply_remote_tow_links(&link, 1, revision, g.world.tick);

    return player->ship->towed_pod_count == 1 &&
           player->ship->towed_pods[0] == pod_idx &&
           cargo_pod_player_tractor(&g.cargo_pod_interp.curr[pod_idx]) ==
               player_idx;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_prepare_npc_scaffold_tether(void) {
    const int npc_idx = 0;
    const int scaffold_idx = MAX_SCAFFOLDS - 1;
    g.local_server.active = false;

    if (!world_npc_ship_slot_activate(&g.world, npc_idx)) return 0;
    npc_ship_t *npc = &g.world.npc_ships[npc_idx];
    npc->active = true;
    npc->role = NPC_ROLE_HAULER;
    npc->ship->pos = v2_add(LOCAL_PLAYER.ship->pos, v2(80.0f, 0.0f));
    npc->ship->vel = v2(0.0f, 0.0f);

    client_npc_render_state_t render_npc = {
        .active = true,
        .role = NPC_ROLE_HAULER,
        .state = NPC_STATE_IDLE,
        .hull_class = npc->ship->hull_class,
        .pos = npc->ship->pos,
        .vel = npc->ship->vel,
        .angle = npc->ship->angle,
        .target_asteroid = -1,
        .towed_fragment = -1,
        .towed_scaffold = scaffold_idx,
        .home_station = -1,
    };
    g.npc_interp.prev[npc_idx] = render_npc;
    g.npc_interp.curr[npc_idx] = render_npc;
    g.npc_interp.elapsed[npc_idx] = 0.0f;

    scaffold_t scaffold = {
        .active = true,
        .state = SCAFFOLD_TOWING,
        .module_type = MODULE_DOCK,
        .owner = -1,
        .pos = v2_add(npc->ship->pos, v2(90.0f, 0.0f)),
        .vel = v2(0.0f, 0.0f),
        .radius = 18.0f,
        .built_at_station = -1,
    };
    g.world.scaffolds[scaffold_idx] = scaffold;
    g.scaffold_interp.prev[scaffold_idx] = scaffold;
    g.scaffold_interp.curr[scaffold_idx] = scaffold;
    g.scaffold_interp.elapsed[scaffold_idx] = 0.0f;

    world_tow_links_clear_source(&g.world, npc->ship_ref);
    if (!world_scaffold_set_npc_tractor(
            &g.world, scaffold_idx, npc_idx)) return 0;
    g.scaffold_interp.prev[scaffold_idx].tractor =
        g.world.scaffolds[scaffold_idx].tractor;
    g.scaffold_interp.curr[scaffold_idx].tractor =
        g.world.scaffolds[scaffold_idx].tractor;
    g.tow_snapshot_received = true;
    g.tow_snapshot_revision = g.world.tow_revision;
    g.tow_snapshot_server_tick = g.world.tow_revision_tick;
    return 1;
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
    /* ACTION_ACK is an immediate transport/dedupe receipt. Authoritative input
     * age is measured from INPUT_APPLIED/private STATE receipts instead. */
    int offset = net_action_queue_find(action_id);
    if (offset >= 0) net_action_queue_remove_at(offset);
    FILE *log = status == NET_ACTION_ACK_REJECTED ? stderr : stdout;
    fprintf(log,
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
    FILE *log = status == NET_ACTION_RESULT_REJECTED ? stderr : stdout;
    fprintf(log,
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
        net_observe_server_tick(server_tick);
        if (!g.net_prediction_tick_valid) {
            net_anchor_prediction_tick(server_tick, false);
        }
    }
    if (action == NET_ACTION_PRESENT_POD &&
        status == NET_ACTION_RESULT_OK) {
        set_notice(
            "Pod cargo unpacked with station-issued receipt continuity.");
    }
    g.action_predict_timer = 0.0f;
}

static void on_remote_input_applied(uint16_t input_seq, uint32_t server_tick,
                                    uint32_t input_tick_ack,
                                    uint32_t client_sent_ms,
                                    uint32_t server_recv_ms,
                                    uint32_t server_send_ms) {
    if (client_sent_ms != 0 && server_recv_ms != 0 &&
        server_send_ms != 0) {
        uint32_t now_ms = net_now_ms32();
        float rtt_ms = (float)(uint32_t)(now_ms - client_sent_ms);
        float server_turnaround_ms =
            (float)(uint32_t)(server_send_ms - server_recv_ms);
        net_observe_transport_latency_sample(rtt_ms, server_turnaround_ms,
                                             server_tick, true);
    }
    net_record_input_ack(input_seq, server_tick, input_tick_ack);
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

void net_observe_transport_latency_sample(float rtt_ms,
                                          float server_turnaround_ms,
                                          uint32_t server_tick,
                                          bool from_input_ack) {
    if (rtt_ms <= 0.0f || rtt_ms > 30000.0f) return;
    float raw_rtt_sec = rtt_ms / 1000.0f;
    float transport_rtt_sec = net_latency_transport_rtt_sec(
        raw_rtt_sec, server_turnaround_ms / 1000.0f);
    if (transport_rtt_sec <= 0.0f) return;
    net_observe_server_tick(server_tick);
    bool dedicated_ping_fresh =
        g.net_last_dedicated_ping_sample_time > 0.0f &&
        g.net_time - g.net_last_dedicated_ping_sample_time <=
            NET_LATENCY_STALE_SEC;
    if (from_input_ack && dedicated_ping_fresh) {
        /* INPUT_APPLIED can be delayed independently by an ordered proxy or
         * congested snapshot lane. Do not let that class-specific delay poison
         * a fresh dedicated PONG estimate. */
        g.net_last_ack_transport_sample_time = g.net_time;
        return;
    }
    g.net_last_ping_raw_rtt = raw_rtt_sec;
    g.net_last_ping_rtt = transport_rtt_sec;
    net_latency_stats_observe(&g.net_ping_latency, transport_rtt_sec,
                              g.net_time);
    g.net_ping_miss_windows_reported = 0;
    g.net_last_ping_server_turnaround_ms = server_turnaround_ms;
    g.net_max_ping_rtt_5s = net_latency_stats_window_max_sec(
        &g.net_ping_latency, g.net_time, NET_PING_MAX_WINDOW_SEC);
    if (g.net_max_ping_rtt_5s <= 0.0f)
        g.net_max_ping_rtt_5s = g.net_last_ping_rtt;
    g.net_ping_samples++;
    if (from_input_ack) {
        g.net_last_ack_transport_sample_time = g.net_time;
    } else {
        g.net_last_dedicated_ping_sample_time = g.net_time;
    }
}

static void on_remote_latency_sample(uint32_t seq, float rtt_ms,
                                     float server_turnaround_ms,
                                     uint32_t server_tick) {
    (void)seq;
    net_observe_transport_latency_sample(rtt_ms, server_turnaround_ms,
                                         server_tick, false);
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
    ship_t *ship = g.world.players[g.local_player_slot].ship;
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
    uint8_t pod_index = g.pending_net_pod_index;
    uint8_t pod_token[32];
    memcpy(pod_token, g.pending_net_pod_token, sizeof(pod_token));

    g.pending_net_action = NET_ACTION_NONE;
    g.pending_net_buy_grade = MINING_GRADE_COUNT;
    g.pending_net_place_station = -1;
    g.pending_net_place_ring = -1;
    g.pending_net_place_slot = -1;
    g.pending_net_pod_index = 0;
    memset(g.pending_net_pod_token, 0,
           sizeof(g.pending_net_pod_token));

    if (net_has_identity_pubkey()) {
        if (!net_has_identity_secret()) {
            fprintf(stderr,
                    "[net-action] blocked action=%u: identity-backed client missing signing secret\n",
                    (unsigned)action);
            set_notice("Signed action unavailable. Secret key missing.");
            return;
        }

        uint16_t action_id = net_next_action_id_alloc();
        if (action == NET_ACTION_PRESENT_POD) {
            uint8_t payload[35] = {0};
            payload[0] = pod_index;
            memcpy(&payload[1], pod_token, sizeof(pod_token));
            payload[33] = (uint8_t)(action_id & 0xFFu);
            payload[34] = (uint8_t)(action_id >> 8);
            if (net_send_signed_action(
                    SIGNED_ACTION_PRESENT_POD,
                    payload, sizeof(payload))) {
                return;
            }
            fprintf(stderr,
                    "[net-action] blocked pod presentation: signed action path rejected for id=%u\n",
                    (unsigned)action_id);
            set_notice("Unable to submit pod presentation.");
            return;
        }
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

    if (action == NET_ACTION_PRESENT_POD) {
        set_notice("Verified identity required to unpack pod cargo.");
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

static void net_track_input_send(uint16_t seq, uint32_t target_tick,
                                 uint32_t sent_ms) {
    if (seq == 0) return;
    int index = (int)(seq % NET_INPUT_TIMING_CAP);
    g.net_input_timing[index].seq = seq;
    g.net_input_timing[index].sent_at = g.net_time;
    g.net_input_timing[index].sent_ms = sent_ms;
    g.net_input_timing[index].target_tick = target_tick;
    g.net_input_timing[index].ping_rtt_at_send =
        net_latency_stats_fresh(&g.net_ping_latency,
                                g.net_time,
                                NET_LATENCY_STALE_SEC)
        ? net_latency_stats_smoothed_sec(&g.net_ping_latency)
        : 0.0f;
}

static void legacy_recovery_ui_update_adapter(void) {
    legacy_recovery_ui_input_t input = {
        .confirm_down =
            is_key_down(SAPP_KEYCODE_ENTER) ||
            is_key_down(SAPP_KEYCODE_KP_ENTER),
        .cancel_down = is_key_down(SAPP_KEYCODE_ESCAPE),
        .confirm_pressed =
            is_key_pressed(SAPP_KEYCODE_ENTER) ||
            is_key_pressed(SAPP_KEYCODE_KP_ENTER),
        .cancel_pressed = is_key_pressed(SAPP_KEYCODE_ESCAPE),
    };
    legacy_recovery_ui_action_t action =
        legacy_recovery_ui_update(
            &legacy_recovery_ui, net_now_ms32(), input);

    if (action == LEGACY_RECOVERY_UI_ACTION_CONFIRM) {
        g.input.key_pressed[SAPP_KEYCODE_ENTER] = false;
        g.input.key_pressed[SAPP_KEYCODE_KP_ENTER] = false;
        bool admitted;
#ifdef __EMSCRIPTEN__
        if (legacy_recovery_smoke_active) {
            legacy_recovery_smoke_confirm_count++;
            admitted = legacy_recovery_smoke_send_admitted;
        } else
#endif
        {
            admitted = net_send_latched_legacy_recovery();
        }
        legacy_recovery_ui_note_send(
            &legacy_recovery_ui, admitted);
        set_notice(
            "%s",
            admitted
                ? "Legacy recovery confirmation sent once; "
                  "verifying atomic commit."
                : "Recovery confirmation was not sent. "
                  "Release and press [ENTER] to retry.");
        return;
    }

    if (action == LEGACY_RECOVERY_UI_ACTION_CANCEL ||
        action == LEGACY_RECOVERY_UI_ACTION_EXPIRE) {
        g.input.key_pressed[SAPP_KEYCODE_ESCAPE] = false;
#ifdef __EMSCRIPTEN__
        if (legacy_recovery_smoke_active) {
            if (action == LEGACY_RECOVERY_UI_ACTION_CANCEL)
                legacy_recovery_smoke_cancel_count++;
            else
                legacy_recovery_smoke_expire_count++;
        }
#endif
        snprintf(
            legacy_recovery_disconnect_notice,
            sizeof(legacy_recovery_disconnect_notice),
            "%s",
            action == LEGACY_RECOVERY_UI_ACTION_CANCEL
                ? "Legacy recovery cancelled locally; "
                  "the remote save is untouched."
                : "Legacy recovery offer expired; "
                  "the remote save is untouched.");
        set_notice("%s", legacy_recovery_disconnect_notice);
#ifdef __EMSCRIPTEN__
        if (legacy_recovery_smoke_active)
            return;
#endif
        net_shutdown();
    }
}

static void frame(void) {
    gameplay_observability_frame_begin();
    double input_started = gameplay_observability_phase_begin();
    float frame_dt = (float)sapp_frame_duration();
    if (!isfinite(frame_dt) || frame_dt < 0.0f)
        frame_dt = 0.0f;
    g.net_time += frame_dt;

    /* --- Network authority: poll incoming and send input before sim. --- */
    if (g.net_authority_enabled) {
        bool was_connected = net_is_connected();
        net_poll();
        handle_net_protocol_mismatch();
        legacy_recovery_ui_update_adapter();
        bool gameplay_ready =
            !g.net_protocol_incompatible &&
            net_is_gameplay_ready() &&
            !legacy_recovery_ui_blocks_gameplay(
                &legacy_recovery_ui);
        if (gameplay_ready) {
            net_update_latency_miss_counters();
            g.net_ping_timer -= frame_dt;
            float ping_interval = net_latency_ping_interval_sec();
            if (g.net_ping_timer > ping_interval)
                g.net_ping_timer = ping_interval;
            if (g.net_ping_timer <= 0.0f) {
                net_send_latency_ping();
                g.net_ping_timer = ping_interval;
            }
            g.net_metrics_timer -= frame_dt;
            if (g.net_metrics_timer <= 0.0f &&
                (g.net_ping_samples > 0 || g.net_motion.total_input_acks > 0)) {
                uint32_t seq = ++g.net_metrics_seq;
                if (seq == 0) seq = ++g.net_metrics_seq;
                float ping_metric_ms = get_net_motion_smoothed_ping_rtt_ms();
                float ack_metric_ms = get_net_motion_smoothed_ack_rtt_ms();
                net_send_client_metrics(
                    seq,
                    ping_metric_ms,
                    ack_metric_ms,
                    get_net_motion_smoothed_ack_gap_ms(),
                    g.net_last_ping_server_turnaround_ms,
                    g.net_motion.packet_interval * 1000.0f,
                    net_unacked_input_count(),
                    g.net_replay_count,
                    g.net_action_queue_count,
                    net_client_recovery_flags());
                g.net_metrics_timer = NET_CLIENT_METRICS_SEC;
            }
        }
        sync_local_player_slot_from_network();
        if (gameplay_ready) {
            net_action_queue_update(frame_dt);
            net_queue_pending_action_if_any();
        }
        if (!g.net_protocol_incompatible &&
            was_connected && !net_is_connected() &&
            !net_is_loopback()) {
            bool preserve_recovery_result =
                legacy_recovery_ui.phase ==
                    LEGACY_RECOVERY_UI_RESULT &&
                legacy_recovery_ui.semantic !=
                    LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS;
            char recovery_notice[
                sizeof(legacy_recovery_disconnect_notice)];
            snprintf(
                recovery_notice, sizeof(recovery_notice), "%s",
                legacy_recovery_disconnect_notice);
            bool recovery_interrupted = recovery_notice[0] != '\0';
            set_notice(
                "%s",
                recovery_interrupted
                    ? recovery_notice
                    : "Connection lost. Press [P] to reconnect.");
            g.local_server.active = false;
            /*
             * Cancellation, expiry, and rejected migrations deliberately
             * close the remote session. Keep their bounded result console
             * alive after disconnect; otherwise the same frame that receives
             * the semantic result would erase it before rendering.
             * A success stays tied to its authoritative remote session.
             */
            if (!preserve_recovery_result)
                legacy_recovery_ui_reset(&legacy_recovery_ui);
            memset(legacy_recovery_disconnect_notice, 0,
                   sizeof(legacy_recovery_disconnect_notice));
        }
        /* P key (offline): hard-reload the page. The HUD prompt is
         * "offline [P] reconnect" but a graceful net_reconnect()
         * never quite worked — sokol_app's browser context, sokol-gl
         * state, and the existing world snapshot all need a clean
         * boot to come back fully consistent. Just refresh. Native
         * builds keep the in-process reconnect path. */
        if (!g.net_protocol_incompatible &&
            !net_is_connected() &&
            is_key_pressed(SAPP_KEYCODE_P)) {
#ifdef __EMSCRIPTEN__
            emscripten_run_script("window.location.reload()");
#else
            if (net_reconnect()) {
                legacy_recovery_ui_reset(&legacy_recovery_ui);
                memset(legacy_recovery_disconnect_notice, 0,
                       sizeof(legacy_recovery_disconnect_notice));
                set_notice("Reconnecting...");
                reset_remote_dynamic_sync();
            }
#endif
        }
        /* Send input immediately when controls change; otherwise keep a
         * low-rate heartbeat. The server persists the last input intent, so
         * unchanged movement does not need a fresh command every frame. */
        if (gameplay_ready) {
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
            input_intent_t network_intent = {0};
            input_sample_network_controls(&network_intent);
            uint8_t flags = input_intent_net_flags(&network_intent);
            uint16_t mining_target =
                ((flags & NET_INPUT_FIRE) != 0 &&
                 LOCAL_PLAYER.hover_asteroid >= 0 &&
                 LOCAL_PLAYER.hover_asteroid < MAX_ASTEROIDS)
                ? (uint16_t)LOCAL_PLAYER.hover_asteroid : 0xFFFFu;
            bool input_changed = !g.net_input_have_last ||
                flags != g.net_last_sent_flags ||
                mining_target != g.net_last_sent_mining_target;
            bool active_controls = flags != 0;
            bool loopback_motion_due = net_is_loopback() && active_controls;
            bool heartbeat_due = g.net_input_timer <= 0.0f;
            g.net_input_ack_timer -= frame_dt;
            uint8_t active_ack_tier = NET_LATENCY_ACK_RECOVERY_STEADY;
            float active_ack_interval = 0.0f;
            if (active_controls) {
                active_ack_interval = net_active_input_ack_interval_sec();
                active_ack_tier = g.net_ack_recovery_tier;
            } else {
                g.net_ack_recovery_tier = NET_LATENCY_ACK_RECOVERY_STEADY;
            }
            if (active_controls &&
                g.net_input_ack_timer > active_ack_interval) {
                g.net_input_ack_timer = active_ack_interval;
            }
            if (input_changed || heartbeat_due || action_due ||
                loopback_motion_due) {
                bool seq_advanced = false;
                bool ack_heartbeat_due = active_controls && heartbeat_due &&
                    g.net_input_ack_timer <= 0.0f;
                if (input_changed || action != NET_ACTION_NONE ||
                    ack_heartbeat_due || loopback_motion_due) {
                    g.net_input_seq++;
                    if (g.net_input_seq == 0) g.net_input_seq++;
                    seq_advanced = true;
                }
                if (action != NET_ACTION_NONE &&
                    net_action_queue_head_first_send()) {
                    net_present_receipt_chains_for_action(action, buy_grade_byte);
                }
                uint32_t input_tick = net_next_input_apply_tick();
                uint32_t input_sent_ms = net_send_input(
                    flags, action, g.net_input_seq, mining_target,
                    buy_grade_byte, place_station, place_ring,
                    place_slot, action_id, input_tick);
                g.net_input_packets_sent++;
                if (seq_advanced) {
                    net_track_input_send(g.net_input_seq, input_tick,
                                         input_sent_ms);
                    if (ack_heartbeat_due &&
                        active_ack_tier != NET_LATENCY_ACK_RECOVERY_STEADY) {
                        g.net_ack_recovery_packets++;
                    }
                    g.net_input_ack_timer = active_controls
                        ? active_ack_interval
                        : 0.0f;
                } else if (!active_controls) {
                    g.net_input_ack_timer = 0.0f;
                }
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

    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_INPUT_NETWORK, input_started);

    double simulation_started = gameplay_observability_phase_begin();
    advance_simulation_frame(frame_dt);
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_SIMULATION, simulation_started);

    /* Offline fallback: keep the client-side manifest summary fresh. The
     * network path fills the summary directly from server packets. */
    if (!g.net_authority_enabled) refresh_station_manifest_summaries();


    double audio_started = gameplay_observability_phase_begin();
    audio_generate_stream(&g.audio);

    /* Upload the latest decoded episode frame once per render frame. Decoding
     * happens inside sim_step (possibly multiple steps per frame); uploading
     * here ensures at most one sg_update_image per image per frame. */
    episode_upload_frame(&g.episode);
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_AUDIO_MEDIA, audio_started);

    render_frame();
    gameplay_observability_frame_end();
}

static void cleanup(void) {
    avatar_shutdown();
    episode_shutdown(&g.episode);
    music_shutdown(&g.music);
    if (g.net_authority_enabled) {
        net_shutdown();
    }
    local_server_shutdown(&g.local_server);
    net_clear_identity();
    identity_clear(&g.identity);
    g.identity_ready = false;
    world_cleanup(&g.world);
    saudio_shutdown();
    sdtx_shutdown();
    hull_fog_shutdown();
    sgl_shutdown();
    sg_shutdown();
    client_log_shutdown();
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
                    if (event->key_code == SAPP_KEYCODE_SPACE)
                        input_tractor_key_down();
                }
            }
            if (event->key_code == SAPP_KEYCODE_ESCAPE && !event->key_repeat &&
                !legacy_recovery_ui_visible(&legacy_recovery_ui) &&
                !g.plan_mode_active &&
                !episode_is_active(&g.episode)) {
                if (hud_dismiss_primary_panel()) {
                    g.input.key_pressed[SAPP_KEYCODE_ESCAPE] = false;
                } else if (!LOCAL_PLAYER.docked &&
                           !g.death_cinematic.active &&
                           g.death_cinematic.menu_alpha <= 0.001f) {
                    sapp_request_quit();
                }
            }
            break;
        }

        case SAPP_EVENTTYPE_KEY_UP: {
            int kc = (int)event->key_code;
            if (kc >= 0 && kc < KEY_COUNT) {
                if (event->key_code == SAPP_KEYCODE_SPACE)
                    input_tractor_key_up();
                g.input.key_down[kc] = false;
            }
            break;
        }

        case SAPP_EVENTTYPE_UNFOCUSED:
#ifdef __EMSCRIPTEN__
            clear_input_state_for_canvas_focus_loss();
#else
            clear_input_state();
#endif
            break;

        case SAPP_EVENTTYPE_SUSPENDED:
        case SAPP_EVENTTYPE_ICONIFIED:
#ifdef __EMSCRIPTEN__
            mobile_clear_virtual_keys();
#endif
            clear_input_state();
            break;

        default:
            break;
    }
}

#ifdef __EMSCRIPTEN__
enum {
    LEGACY_RECOVERY_CTRL_VISIBLE     = 1u << 0,
    LEGACY_RECOVERY_CTRL_CAN_CONFIRM = 1u << 1,
    LEGACY_RECOVERY_CTRL_CAN_CANCEL  = 1u << 2,
    LEGACY_RECOVERY_CTRL_CONFIRMING  = 1u << 3,
    LEGACY_RECOVERY_CTRL_RESULT      = 1u << 4,
    LEGACY_RECOVERY_CTRL_SUCCESS     = 1u << 5,
};

EMSCRIPTEN_KEEPALIVE
int signal_legacy_recovery_ui_flags(void) {
    uint32_t flags = 0;
    if (legacy_recovery_ui_visible(&legacy_recovery_ui))
        flags |= LEGACY_RECOVERY_CTRL_VISIBLE;
    if (legacy_recovery_ui_can_confirm(&legacy_recovery_ui))
        flags |= LEGACY_RECOVERY_CTRL_CAN_CONFIRM;
    if (legacy_recovery_ui_can_cancel(&legacy_recovery_ui))
        flags |= LEGACY_RECOVERY_CTRL_CAN_CANCEL;
    if (legacy_recovery_ui.phase == LEGACY_RECOVERY_UI_CONFIRMING)
        flags |= LEGACY_RECOVERY_CTRL_CONFIRMING;
    if (legacy_recovery_ui.phase == LEGACY_RECOVERY_UI_RESULT)
        flags |= LEGACY_RECOVERY_CTRL_RESULT;
    if (legacy_recovery_ui.semantic ==
        LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS) {
        flags |= LEGACY_RECOVERY_CTRL_SUCCESS;
    }
    return (int)flags;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_legacy_recovery_ui_semantic(void) {
    return legacy_recovery_ui_semantic_name(
        legacy_recovery_ui.semantic);
}

EMSCRIPTEN_KEEPALIVE
const char *signal_legacy_recovery_ui_copy(void) {
    static char copy[768];
    snprintf(
        copy, sizeof(copy), "%s\n%s\n%s\n%s",
        legacy_recovery_ui_title(&legacy_recovery_ui),
        legacy_recovery_ui_status(&legacy_recovery_ui),
        legacy_recovery_ui_body(&legacy_recovery_ui),
        legacy_recovery_ui_detail(&legacy_recovery_ui));
    return copy;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_legacy_recovery_offer(int expires_in_seconds) {
    if (expires_in_seconds <= 0 ||
        expires_in_seconds > UINT16_MAX) {
        return 0;
    }
    legacy_recovery_ui_reset(&legacy_recovery_ui);
    memset(legacy_recovery_disconnect_notice, 0,
           sizeof(legacy_recovery_disconnect_notice));
    legacy_recovery_smoke_active = true;
    legacy_recovery_smoke_send_admitted = true;
    legacy_recovery_smoke_confirm_count = 0;
    legacy_recovery_smoke_cancel_count = 0;
    legacy_recovery_smoke_expire_count = 0;
    g.input.key_pressed[SAPP_KEYCODE_ENTER] = false;
    g.input.key_pressed[SAPP_KEYCODE_KP_ENTER] = false;
    g.input.key_pressed[SAPP_KEYCODE_ESCAPE] = false;
    return legacy_recovery_ui_begin_offer(
        &legacy_recovery_ui, net_now_ms32(),
        (uint16_t)expires_in_seconds) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_legacy_recovery_result(int status) {
    legacy_recovery_ui_phase_t before =
        legacy_recovery_ui.phase;
    on_remote_legacy_recovery_result(
        (legacy_recovery_result_status_t)status);
    return before != legacy_recovery_ui.phase &&
        legacy_recovery_ui.phase == LEGACY_RECOVERY_UI_RESULT;
}

EMSCRIPTEN_KEEPALIVE
void signal_smoke_legacy_recovery_set_send_admitted(int admitted) {
    legacy_recovery_smoke_send_admitted = admitted != 0;
}

EMSCRIPTEN_KEEPALIVE
uint32_t signal_smoke_legacy_recovery_confirm_count(void) {
    return legacy_recovery_smoke_confirm_count;
}

EMSCRIPTEN_KEEPALIVE
uint32_t signal_smoke_legacy_recovery_cancel_count(void) {
    return legacy_recovery_smoke_cancel_count;
}

EMSCRIPTEN_KEEPALIVE
uint32_t signal_smoke_legacy_recovery_expire_count(void) {
    return legacy_recovery_smoke_expire_count;
}

EMSCRIPTEN_KEEPALIVE
void signal_smoke_legacy_recovery_reset(void) {
    legacy_recovery_ui_reset(&legacy_recovery_ui);
    memset(legacy_recovery_disconnect_notice, 0,
           sizeof(legacy_recovery_disconnect_notice));
    legacy_recovery_smoke_active = false;
    legacy_recovery_smoke_send_admitted = true;
    legacy_recovery_smoke_confirm_count = 0;
    legacy_recovery_smoke_cancel_count = 0;
    legacy_recovery_smoke_expire_count = 0;
}

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
    MOBILE_CTRL_CAN_LINEAGE       = 1u << 30,
};
#define MOBILE_CTRL_LINEAGE_OPEN UINT32_C(0x80000000)

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
        case STATION_VIEW_TRADE:
            flags |= MOBILE_CTRL_STATION_TRADE;
            if (trade_lineage_available(st, LOCAL_PLAYER.ship))
                flags |= MOBILE_CTRL_CAN_LINEAGE;
            if (g.trade_lineage_row >= 0) {
                flags |= MOBILE_CTRL_LINEAGE_OPEN;
                if (g.trade_lineage_proof) flags |= MOBILE_CTRL_CAN_PAGE;
            } else {
                flags |= MOBILE_CTRL_CAN_PAGE |
                         MOBILE_CTRL_CAN_SELL |
                         MOBILE_CTRL_CAN_DIGITS;
            }
            break;
        case STATION_VIEW_WORK:  flags |= MOBILE_CTRL_STATION_WORK |
                                       MOBILE_CTRL_CAN_SELL |
                                       MOBILE_CTRL_CAN_DIGITS; break;
        case STATION_VIEW_YARD:  flags |= MOBILE_CTRL_STATION_YARD |
                                       MOBILE_CTRL_CAN_DIGITS; break;
        case STATION_VIEW_HISTORY:
            flags |= MOBILE_CTRL_CAN_DIGITS;
            break;
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
        if (!g.plan_mode_active && LOCAL_PLAYER.ship->towed_scaffold < 0)
            flags |= MOBILE_CTRL_CAN_PLAN;
    }

    if (LOCAL_PLAYER.in_dock_range) flags |= MOBILE_CTRL_IN_DOCK_RANGE;
    if (LOCAL_PLAYER.docking_approach) flags |= MOBILE_CTRL_DOCKING_APPROACH;
    if (ship_total_cargo(LOCAL_PLAYER.ship) > 0.0f)
        flags |= MOBILE_CTRL_HAS_CARGO;

    if (g.plan_mode_active) {
        flags |= MOBILE_CTRL_PLAN_ACTIVE | MOBILE_CTRL_CAN_USE |
                 MOBILE_CTRL_CAN_PLAN | MOBILE_CTRL_CAN_CYCLE;
        if (g.plan_target_station < 0) flags |= MOBILE_CTRL_PLAN_GHOST;
    }

    if (!docked && LOCAL_PLAYER.ship->towed_scaffold >= 0)
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
    } else if (signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos) >=
               SIGNAL_BAND_OPERATIONAL) {
        flags |= MOBILE_CTRL_AUTOPILOT_READY;
    }

    return (int)flags;
}

static const station_panel_descriptor_t *mobile_active_station_panel(void) {
    if (!LOCAL_PLAYER.docked) return NULL;
    const station_t *st = current_station_ptr();
    if (!st) return NULL;
    const station_panel_descriptor_t *panel =
        station_panel_descriptor(g.station_view);
    if (!station_panel_visible(panel, st)) {
        g.station_view = station_panel_first_visible(st);
        panel = station_panel_descriptor(g.station_view);
    }
    return panel;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_station_panel_label(void) {
    const station_panel_descriptor_t *panel = mobile_active_station_panel();
    return (panel && panel->label) ? panel->label : "";
}

EMSCRIPTEN_KEEPALIVE
const char *signal_hud_attention_surface(void) {
    return hud_attention_current_surface_name();
}

EMSCRIPTEN_KEEPALIVE
int signal_hud_debug_visible(void) {
    return g.hud_debug_visible ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_hud_scan_asteroid_budget(void) {
    return hud_scan_asteroid_budget(ui_screen_width());
}

EMSCRIPTEN_KEEPALIVE
int signal_hud_scan_npc_budget(void) {
    return hud_scan_npc_budget(ui_screen_width());
}

EMSCRIPTEN_KEEPALIVE
const char *signal_station_panel_legend(void) {
    const station_panel_descriptor_t *panel = mobile_active_station_panel();
    static char legend[96];
    legend[0] = '\0';
    const station_t *st = current_station_ptr();
    if (panel && station_panel_legend_text(panel->view, st, legend,
                                           sizeof(legend))) {
        return legend;
    }
    return "";
}

EMSCRIPTEN_KEEPALIVE
const char *signal_trade_lineage_text(void) {
    static char text[8192];
    text[0] = '\0';
    (void)trade_lineage_selected_text(text, sizeof(text));
    return text;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_station_credit_perception_summary(void) {
    static char summary[384];
    smoke_apply_loop_state_for_frame();
    summary[0] = '\0';
    (void)station_credit_perception_summary(summary, sizeof(summary));
    return summary;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_signal_loss_perception_summary(void) {
    static char summary[96];
    summary[0] = '\0';
    (void)hud_signal_loss_perception_summary(summary, sizeof(summary));
    return summary;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_npc_motive_perception_summary(void) {
    static char summary[320];
    summary[0] = '\0';
    (void)hud_npc_motive_perception_summary(summary, sizeof(summary));
    return summary;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_remembered_work_perception_summary(void) {
    static char summary[384];
    summary[0] = '\0';
    (void)station_remembered_work_summary(summary, sizeof(summary));
    return summary;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_laser_refit_summary(void) {
    static char summary[256];
    summary[0] = '\0';
    (void)station_laser_refit_summary(summary, sizeof(summary));
    return summary;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_station_production_summary(void) {
    static char summary[256];
    summary[0] = '\0';
    (void)station_production_summary(summary, sizeof(summary));
    return summary;
}

EMSCRIPTEN_KEEPALIVE
int signal_station_panel_digit_slot_count(void) {
    if (!LOCAL_PLAYER.docked) return 0;
    const station_t *st = current_station_ptr();
    if (!st || !mobile_active_station_panel()) return 0;

    switch (g.station_view) {
    case STATION_VIEW_TRADE: {
        if (g.trade_lineage_row >= 0) return 0;
        trade_row_t rows[TRADE_MAX_ROWS];
        int row_count = build_trade_rows(st, LOCAL_PLAYER.ship, rows, TRADE_MAX_ROWS);
        int page_first = 0, page_last = 0, total_pages = 1;
        trade_page_range(rows, row_count, (int)g.trade_page,
                         &page_first, &page_last, &total_pages);
        if ((int)g.trade_page >= total_pages) {
            trade_page_range(rows, row_count, 0,
                             &page_first, &page_last, &total_pages);
        }
        int n = page_last - page_first;
        if (n < 0) n = 0;
        return n > 5 ? 5 : n;
    }
    case STATION_VIEW_WORK: {
        int slot_contract[3] = {-1, -1, -1};
        bool slot_fulfillable[3] = {false, false, false};
        int slot_held[3] = {0, 0, 0};
        int n = build_work_slots(LOCAL_PLAYER.current_station, st->pos,
                                 slot_contract, slot_fulfillable, slot_held);
        if (n < 0) n = 0;
        return n > 3 ? 3 : n;
    }
    case STATION_VIEW_YARD: {
        if (!station_has_module(st, MODULE_SHIPYARD)) return 0;
        int shown = 0;
        for (int t = 0; t < MODULE_COUNT && shown < 5; t++) {
            module_type_t kit = (module_type_t)t;
            if (module_kind(kit) == MODULE_KIND_NONE) continue;
            if (!station_has_module(st, kit)) continue;
            if (!module_unlocked_for_player(LOCAL_PLAYER.ship->unlocked_modules, kit))
                continue;
            shown++;
        }
        return shown;
    }
    case STATION_VIEW_HISTORY:
        return 4;
    case STATION_VIEW_DOCK:
    case STATION_VIEW_COUNT:
        break;
    }
    return 0;
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
        if (g.trade_lineage_row >= 0) break;
        trade_row_t rows[TRADE_MAX_ROWS];
        int row_count = build_trade_rows(st, LOCAL_PLAYER.ship, rows, TRADE_MAX_ROWS);
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
            if (!module_unlocked_for_player(LOCAL_PLAYER.ship->unlocked_modules, kit))
                continue;
            mask |= (1 << shown);
            shown++;
        }
        break;
    }
    case STATION_VIEW_HISTORY:
        mask = 0x0F;
        break;
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
    case 17: return SAPP_KEYCODE_L;          /* cargo lineage / next cargo */
    case 18: return SAPP_KEYCODE_I;          /* cargo proof / story toggle */
    case 20: return SAPP_KEYCODE_1;
    case 21: return SAPP_KEYCODE_2;
    case 22: return SAPP_KEYCODE_3;
    case 23: return SAPP_KEYCODE_4;
    case 24: return SAPP_KEYCODE_5;
    case 30: return SAPP_KEYCODE_O;          /* autopilot */
    case 31: return SAPP_KEYCODE_ESCAPE;     /* back / close */
    case 32: return SAPP_KEYCODE_ENTER;      /* confirm recovery */
    default: return SAPP_KEYCODE_INVALID;
    }
}

EMSCRIPTEN_KEEPALIVE
void signal_mobile_key(int action, int down) {
    int kc = (int)mobile_action_key(action);
    if (kc < 0 || kc >= KEY_COUNT) return;

    if (down) {
        g_mobile_virtual_key_down[kc] = true;
        if (!g.input.key_down[kc]) {
            g.input.key_pressed[kc] = true;
            if (kc == SAPP_KEYCODE_SPACE) input_tractor_key_down();
        }
        g.input.key_down[kc] = true;
    } else {
        g_mobile_virtual_key_down[kc] = false;
        if (kc == SAPP_KEYCODE_SPACE) input_tractor_key_up();
        g.input.key_down[kc] = false;
    }
}

EMSCRIPTEN_KEEPALIVE
void signal_mobile_clear(void) {
    mobile_clear_virtual_keys();
    clear_input_state();
}

EMSCRIPTEN_KEEPALIVE
int signal_debug_held_control_mask(void) {
    int mask = 0;
    if (is_key_down(SAPP_KEYCODE_W) || is_key_down(SAPP_KEYCODE_UP))
        mask |= 1 << 0;
    if (is_key_down(SAPP_KEYCODE_S) || is_key_down(SAPP_KEYCODE_DOWN))
        mask |= 1 << 1;
    if (is_key_down(SAPP_KEYCODE_A) || is_key_down(SAPP_KEYCODE_LEFT))
        mask |= 1 << 2;
    if (is_key_down(SAPP_KEYCODE_D) || is_key_down(SAPP_KEYCODE_RIGHT))
        mask |= 1 << 3;
    if (is_key_down(SAPP_KEYCODE_M))
        mask |= 1 << 4;
    if (is_key_down(SAPP_KEYCODE_SPACE))
        mask |= 1 << 5;
    if (is_key_down(SAPP_KEYCODE_LEFT_SHIFT) || is_key_down(SAPP_KEYCODE_RIGHT_SHIFT))
        mask |= 1 << 6;
    return mask;
}

int signal_debug_identity_available(void) {
    return g.identity_ready ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_debug_player_progress(void) {
    return (int)g.worker_story.flags |
        ((int)client_progress_current().guide << 8);
}

int signal_debug_auth_available(void) {
    return (g.identity_ready && g.net_authority_enabled) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_debug_net_connected(void) {
    return net_is_connected() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_market_memory_packet_parity(void) {
    if (!LOCAL_PLAYER.ship) return 0;
    knowledge_view_t saved = LOCAL_PLAYER.ship->knowledge;
    NetMarketMemoryEntry entry = {0};
    entry.memory.active = true;
    entry.memory.memory_kind = (uint8_t)MARKET_MEMORY_SUPPLY;
    entry.memory.station_a = 1;
    entry.memory.station_b = 2;
    entry.memory.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    entry.memory.action = 0xFFu;
    entry.memory.confidence = 201;
    entry.memory.salience = 177;
    entry.memory.quantity_hint = 12;
    entry.memory.value_hint = 345;
    entry.memory.observed_tick = 6789;
    entry.memory.subject_nonce = 0x12345678u;
    entry.hops = 3;

    apply_remote_player_market_memories(&entry, 1);
    const knowledge_view_t *view = &LOCAL_PLAYER.ship->knowledge;
    market_memory_t decoded = {0};
    if (view->count == 1)
        memcpy(&decoded, view->items[0].payload, sizeof(decoded));
    bool ok = view->count == 1 &&
              view->items[0].kind == (uint8_t)KNOW_MARKET &&
              view->items[0].payload_kind ==
                  (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY &&
              view->items[0].hops == 3 &&
              decoded.active &&
              decoded.memory_kind == (uint8_t)MARKET_MEMORY_SUPPLY &&
              decoded.station_a == 1 && decoded.station_b == 2 &&
              decoded.quantity_hint == 12 && decoded.value_hint == 345 &&
              decoded.observed_tick == 6789;
    LOCAL_PLAYER.ship->knowledge = saved;
    return ok ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_debug_local_authority_state(void) {
    int state = 0;
    if (local_server_has_world(&g.local_server)) state |= 1 << 0;
    if (g.local_server.active) state |= 1 << 1;
    if (net_is_loopback()) state |= 1 << 2;
    if (g.net_authority_enabled) state |= 1 << 3;
    return state;
}

EMSCRIPTEN_KEEPALIVE
int signal_debug_local_authority_generation(void) {
    return (int)g.local_server.generation;
}

EMSCRIPTEN_KEEPALIVE
int signal_debug_restart_local_authority(void) {
    NetCallbacks cbs;
    configure_net_callbacks(&cbs);
    return start_fresh_local_authority(&cbs) ? 1 : 0;
}
#endif

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    (void)client_log_init();
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
