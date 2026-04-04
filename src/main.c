#include <stdarg.h>
#include <stdlib.h>

#include "client.h"
#include "audio.h"
#include "npc.h"
#include "render.h"
#include "rng.h"
#include "asteroid_field.h"
#include "net.h"
#include "world_draw.h"
#include "input.h"
#include "net_sync.h"
#include "onboarding.h"

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

/* Audio mix callback: blends episode video audio + music into SFX output */
static void mix_external_audio(float *buffer, int frames, int channels, void *user) {
    (void)user;
    episode_read_audio(&g.episode, buffer, frames, channels);
    music_read_audio(&g.music, buffer, frames, channels);
}


/* clear_input_state, consume_pressed_input, set_notice: see input.h/c */

/* asteroid_next_tier, asteroid_is_collectible, asteroid_progress_ratio: see asteroid.h/c */

/* commodity_refined_form, commodity_name, commodity_code, commodity_short_name: see commodity.h/c */

/* ship_total_cargo, ship_raw_ore_total, ship_cargo_amount, station_buy_price, station_inventory_amount: see commodity.h/c */

/* format_ore_manifest ... format_refinery_price_line: see station_ui.c */
/* station_at ... navigation_station_ptr: see station_ui.c */
/* station_role_name, station_role_short_name: see station_ui.c */
/* station_has_service, station_upgrade_service: see station_ui.c */
/* build_station_ui_state, format_station_* helpers: see station_ui.c */
/* station_role_hub_label, station_role_market_title, station_role_fit_title: see station_ui.c */
/* station_role_color: see station_ui.c */
/* build_station_service_lines, draw_station_service_text_line: see station_ui.c */
/* can_afford_upgrade: see economy.h/c */

/* station_dock_anchor, ship_cargo_space: see game_sim.c */

static void clear_collection_feedback(void) {
    g.collection_feedback_ore = 0.0f;
    g.collection_feedback_fragments = 0;
    g.collection_feedback_timer = 0.0f;
}

/* random_field_asteroid_tier, client_max_signal_range, spawn_field_asteroid_of_tier,
 * spawn_field_asteroid, spawn_child_asteroid, desired_child_count,
 * inspect_asteroid_field: see asteroid_field.h/c */

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
    if (!g.multiplayer_enabled) {
        /* Singleplayer: use local server as authoritative sim */
        local_server_init(&g.local_server, 0);
        /* Copy full initial state to client world view */
        g.world = g.local_server.world;
    } else {
        /* Multiplayer: server manages world, client just predicts */
        world_reset(&g.world);
        player_init_ship(&LOCAL_PLAYER, &g.world);
        LOCAL_PLAYER.connected = true;
    }

    g.local_player_slot = 0;
    g.tracked_contract = -1;
    g.target_station = -1;
    g.target_module = -1;
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    g.asteroid_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    g.npc_interp.interval = g.local_server.active ? SIM_DT : 0.1f;
    memset(&g.player_interp, 0, sizeof(g.player_interp));
    g.player_interp.interval = g.local_server.active ? SIM_DT : 0.1f;

    /* Seed interp buffers so first frame has valid data */
    memcpy(g.asteroid_interp.curr, g.world.asteroids, sizeof(g.asteroid_interp.curr));
    memcpy(g.asteroid_interp.prev, g.world.asteroids, sizeof(g.asteroid_interp.prev));
    memcpy(g.npc_interp.curr, g.world.npc_ships, sizeof(g.npc_interp.curr));
    memcpy(g.npc_interp.prev, g.world.npc_ships, sizeof(g.npc_interp.prev));

    g.thrusting = false;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    audio_clear_voices(&g.audio);
    clear_collection_feedback();

    set_notice("%s online. Press E to launch.", g.world.stations[LOCAL_PLAYER.current_station].name);
}

/* Camera/frustum, asteroid_profile, draw_background, draw_station, draw_ship*,
 * draw_npc_*, draw_beam, draw_remote_players: see world_draw.h/c */

/* draw_ui_scanlines ... draw_hud: see hud.c */
/* draw_station_services: see station_ui.c */


/* is_key_down, is_key_pressed: see input.h/c */

/* ship_forward, ship_muzzle: see ship.h/c */

static void reset_step_feedback(void) {
    LOCAL_PLAYER.hover_asteroid = -1;
    LOCAL_PLAYER.beam_active = false;
    LOCAL_PLAYER.beam_hit = false;
    g.thrusting = false;
    /* nearby/tractor fragment counts are reset inside step_fragment_collection
     * on the authoritative sim — don't zero them here, the sync carries them. */
}

/* sample_input_intent: see input.h/c */

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
}

/* step_refinery_production, step_station_production: see economy.h/c */

/* No sync_globals_to_world — world_t is the source of truth in single player. */

/* sync_world_to_globals removed — everything reads from g.world directly */

static void process_sim_events(const sim_events_t *events) {
    for (int i = 0; i < events->count; i++) {
        const sim_event_t* ev = &events->events[i];
        switch (ev->type) {
            case SIM_EVENT_FRACTURE:
                audio_play_fracture(&g.audio, ev->fracture.tier);
                break;
            case SIM_EVENT_MINING_TICK:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_mining_tick(&g.audio);
                    onboarding_mark_mined();
                }
                break;
            case SIM_EVENT_DOCK:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_dock(&g.audio);
                    set_notice("Docked at %s.", g.world.stations[LOCAL_PLAYER.current_station].name);
                    /* Track visited original stations for Ep 1 */
                    int ds = LOCAL_PLAYER.current_station;
                    if (ds < 3) {
                        g.episode.stations_visited |= (1 << ds);
                        if (g.episode.stations_visited == 7) /* all 3 */
                            episode_trigger(&g.episode, 1); /* Ep 1: Kepler's Law */
                    }
                }
                break;
            case SIM_EVENT_LAUNCH:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_launch(&g.audio);
                    set_notice("Launch corridor clear.");
                    onboarding_mark_launched();
                    episode_trigger(&g.episode, 0); /* Ep 0: First Light */
                    /* Start music on first launch */
                    if (!g.music.playing && !g.music.loading)
                        music_play(&g.music, 0);
                }
                break;
            case SIM_EVENT_SELL:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_sale(&g.audio);
                    onboarding_mark_sold();
                    episode_trigger(&g.episode, 2); /* Ep 2: Furnace — first smelt */
                }
                break;
            case SIM_EVENT_REPAIR:
                if (ev->player_id == g.local_player_slot) audio_play_repair(&g.audio);
                break;
            case SIM_EVENT_UPGRADE:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_upgrade(&g.audio, ev->upgrade.upgrade);
                    onboarding_mark_upgraded();
                }
                break;
            case SIM_EVENT_DAMAGE:
                if (ev->player_id == g.local_player_slot) audio_play_damage(&g.audio, ev->damage.amount);
                break;
            case SIM_EVENT_CONTRACT_COMPLETE:
                if (ev->contract_complete.action == CONTRACT_SUPPLY) {
                    set_notice("Supply contract fulfilled.");
                    episode_trigger(&g.episode, 6); /* Ep 6: Hauler */
                } else if (ev->contract_complete.action == CONTRACT_DESTROY) {
                    set_notice("Target destroyed. Contract complete.");
                }
                break;
            case SIM_EVENT_DEATH:
                if (ev->player_id == g.local_player_slot) {
                    g.death_screen_timer = 4.0f;
                    g.death_ore_mined = ev->death.ore_mined;
                    g.death_credits_earned = ev->death.credits_earned;
                    g.death_credits_spent = ev->death.credits_spent;
                    g.death_asteroids_fractured = ev->death.asteroids_fractured;
                    episode_trigger(&g.episode, 9); /* Ep 9: Death */
                    /* Reset episode milestones — replay on next life */
                    memset(g.episode.watched, 0, sizeof(g.episode.watched));
                    g.episode.stations_visited = 0;
                    episode_save(&g.episode);
                }
                break;
            default:
                break;
        }
    }
}

static void onboarding_per_frame(void) {
    if (g.onboarding.complete) return;
    if (!g.onboarding.collected && ship_total_cargo(&LOCAL_PLAYER.ship) > 0.5f)
        onboarding_mark_collected();
    if (!g.onboarding.bought) {
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
            if (LOCAL_PLAYER.ship.cargo[c] > 0.5f) { onboarding_mark_bought(); break; }
    }
    if (!g.onboarding.got_scaffold && LOCAL_PLAYER.ship.has_scaffold_kit)
        onboarding_mark_got_scaffold();
    if (!g.onboarding.placed_outpost) {
        for (int s = 3; s < MAX_STATIONS; s++)
            if (station_exists(&g.world.stations[s])) { onboarding_mark_placed_outpost(); break; }
    }
}

static void episode_per_frame(void) {
    if (episode_is_active(&g.episode)) return;

    /* Ep 3: Scaffold — bought first scaffold kit */
    if (!g.episode.watched[3] && g.onboarding.got_scaffold)
        episode_trigger(&g.episode, 3);

    /* Ep 4: Naming — placed first outpost */
    if (!g.episode.watched[4] && g.onboarding.placed_outpost)
        episode_trigger(&g.episode, 4);

    /* Ep 5: Drones — triggered server-side when NPC spawns at player outpost (TODO) */

    /* Ep 7: Dark Sector — enter zero-signal space */
    if (!g.episode.watched[7]) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig < 0.01f)
            episode_trigger(&g.episode, 7);
    }

    /* Ep 8: Every AI Dreams — 5+ connected stations */
    if (!g.episode.watched[8]) {
        int connected = 0;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (station_exists(&g.world.stations[s]) && g.world.stations[s].signal_connected)
                connected++;
        }
        if (connected >= 5)
            episode_trigger(&g.episode, 8);
    }
}

static void sim_step(float dt) {
    reset_step_feedback();
    audio_step(&g.audio, dt);

    /* Derive ring rotation from world time (multiplayer only —
     * singleplayer gets rotation from local server sync).
     * Using world.time ensures client and server produce identical
     * rotation, preventing collision/visual desync. */
    if (g.multiplayer_enabled) {
        for (int s = 0; s < MAX_STATIONS; s++) {
            station_t *st = &g.world.stations[s];
            if (!station_exists(st)) continue;
            float base = st->arm_speed[0] * g.world.time;
            base = base - floorf(base / TWO_PI_F) * TWO_PI_F;
            for (int r = 0; r < STATION_NUM_RINGS && r < MAX_ARMS; r++) {
                st->arm_rotation[r] = base;
            }
        }
    }

    /* Death screen countdown — block all input while active */
    if (g.death_screen_timer > 0.0f) {
        g.death_screen_timer = fmaxf(0.0f, g.death_screen_timer - dt);
        consume_pressed_input();
        return;
    }

    input_intent_t intent = sample_input_intent();

    /* Tab switching while docked */
    if (LOCAL_PLAYER.docked && !g.was_docked) {
        /* Just docked — reset to overview (or construction for scaffolds) */
        const station_t* st = &g.world.stations[LOCAL_PLAYER.current_station];
        g.station_tab = STATION_TAB_STATUS;
        g.build_overlay = false;
        g.placing_outpost = false;
        /* Clear blueprint pip if we docked at the blueprint station */
        if (g.nav_pip_is_blueprint) {
            float d = sqrtf(v2_dist_sq(st->pos, g.nav_pip_pos));
            if (d < 200.0f) {
                g.nav_pip_is_blueprint = false;
                g.nav_pip_pos = st->pos;
            }
            /* Otherwise keep the blueprint pip active */
        } else {
            g.nav_pip_active = true;
            g.nav_pip_pos = st->pos;
        }
    }
    if (LOCAL_PLAYER.docked && !g.build_overlay && (is_key_pressed(SAPP_KEYCODE_TAB) || is_key_pressed(SAPP_KEYCODE_Q))) {
        station_tab_t vtabs[STATION_TAB_COUNT];
        int vtab_count = 0;
        vtabs[vtab_count++] = STATION_TAB_STATUS;
        vtabs[vtab_count++] = STATION_TAB_MARKET;
        vtabs[vtab_count++] = STATION_TAB_CONTRACTS;
        int cur = 0;
        for (int i = 0; i < vtab_count; i++) { if (vtabs[i] == g.station_tab) { cur = i; break; } }
        int dir = is_key_pressed(SAPP_KEYCODE_TAB) ? 1 : (vtab_count - 1);
        g.station_tab = vtabs[(cur + dir) % vtab_count];
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
            /* Only reload once — if URL already has ?v= we already tried */
            int already_tried = emscripten_run_script_int(
                "location.search.indexOf('v=') >= 0 ? 1 : 0");
            if (!already_tried) {
                emscripten_run_script("location.replace(location.pathname + '?v=' + Date.now())");
            }
#endif
        }
    }

    /* Track dock transition AFTER server sync to prevent was_docked flicker */
    g.was_docked = LOCAL_PLAYER.docked;

    /* Advance interpolation timers (both modes) */
    g.asteroid_interp.t += dt / fmaxf(g.asteroid_interp.interval, 0.01f);
    g.npc_interp.t += dt / fmaxf(g.npc_interp.interval, 0.01f);
    g.player_interp.t += dt / fmaxf(g.player_interp.interval, 0.01f);

    g.thrusting = (intent.thrust > 0.0f) && !LOCAL_PLAYER.docked;

    /* Play audio from sim events (singleplayer only — multiplayer has no server events) */
    process_sim_events(&g.world.events);

    /* Detect state transitions for music/episode triggers (works in both modes) */
    if (g.was_docked && !LOCAL_PLAYER.docked) {
        /* Just launched */
        episode_trigger(&g.episode, 0);
        if (!g.music.playing && !g.music.loading)
            music_play(&g.music, 0);
    }
    if (!g.was_docked && LOCAL_PLAYER.docked) {
        /* Just docked */
        int ds = LOCAL_PLAYER.current_station;
        if (ds < 3) {
            g.episode.stations_visited |= (1 << ds);
            if (g.episode.stations_visited == 7)
                episode_trigger(&g.episode, 1);
        }
    }

    onboarding_per_frame();
    episode_per_frame();
    episode_update(&g.episode, dt);
    music_update(&g.music, dt);

    /* ESC dismisses episode popup */
    if (episode_is_active(&g.episode) && g.input.key_pressed[SAPP_KEYCODE_ESCAPE])
        episode_skip(&g.episode);

    /* Music controls: M = mute/unmute, [ = prev, ] = next */
    if (g.input.key_pressed[SAPP_KEYCODE_M]) {
        if (g.music.playing)
            g.music.paused ? music_resume(&g.music) : music_pause(&g.music);
        else
            music_play(&g.music, 0);
    }
    if (g.input.key_pressed[SAPP_KEYCODE_RIGHT_BRACKET] && g.music.playing)
        music_next_track(&g.music);
    if (g.input.key_pressed[SAPP_KEYCODE_LEFT_BRACKET] && g.music.playing) {
        int prev = (g.music.current_track - 1 + MUSIC_TRACK_COUNT) % MUSIC_TRACK_COUNT;
        music_play(&g.music, prev);
    }

    step_notice_timer(dt);
    if (g.action_predict_timer > 0.0f)
        g.action_predict_timer = fmaxf(0.0f, g.action_predict_timer - dt);

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

    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value = (sg_color){ 0.018f, 0.024f, 0.045f, 1.0f };

    init_starfield();
    reset_world();
    onboarding_load();

    /* --- Multiplayer: auto-connect if server URL is available --- */
    {
        const char* server_url = NULL;
#ifdef __EMSCRIPTEN__
        server_url = emscripten_run_script_string(
            "(() => {"
            "  const p = new URLSearchParams(window.location.search);"
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
            cbs.on_player_ship = apply_remote_player_ship;
            cbs.on_contracts = apply_remote_contracts;
            g.multiplayer_enabled = net_init(server_url, &cbs);
            if (g.multiplayer_enabled) {
                /* Deactivate the local server — the remote server is authoritative.
                 * The local server was started by reset_world() before we knew
                 * multiplayer was available. */
                g.local_server.active = false;
            }
        }
    }
}


/* on_player_join ... sync_local_player_slot_from_network: see net_sync.h/c */

static void render_world(void) {
    vec2 camera = LOCAL_PLAYER.ship.pos;
    float half_w = sapp_widthf() * 0.5f;
    float half_h = sapp_heightf() * 0.5f;

    set_camera_bounds(camera, half_w, half_h);

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(cam_left(), cam_right(), cam_top(), cam_bottom(), -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    draw_background(camera);

    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t* st = &g.world.stations[i];
        if (!station_exists(st) && !st->scaffold) continue;
        if (!on_screen(st->pos.x, st->pos.y, st->dock_radius + 20.0f)) continue;
        bool is_current = LOCAL_PLAYER.docked && (i == LOCAL_PLAYER.current_station);
        bool is_nearby = (!LOCAL_PLAYER.docked) && (i == LOCAL_PLAYER.nearby_station);
        draw_station(st, is_current, is_nearby);
    }
    /* Outpost placement preview */
    if (g.placing_outpost && !LOCAL_PLAYER.docked) {
        vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
        vec2 target = v2_add(LOCAL_PLAYER.ship.pos, v2_scale(forward, 150.0f));
        bool valid = can_place_outpost(&g.world, target);
        float cr = valid ? 0.3f : 0.9f;
        float cg = valid ? 0.9f : 0.2f;
        float cb = valid ? 0.5f : 0.2f;
        float pulse = 0.5f + 0.3f * sinf(g.world.time * 4.0f);
        draw_circle_outline(target, OUTPOST_RADIUS, 18, cr, cg, cb, pulse);
        draw_circle_outline(target, OUTPOST_DOCK_RADIUS, 24, cr * 0.6f, cg * 0.6f, cb * 0.6f, pulse * 0.5f);
        /* Crosshair */
        draw_segment(v2_add(target, v2(-20.0f, 0.0f)), v2_add(target, v2(20.0f, 0.0f)), cr, cg, cb, pulse * 0.7f);
        draw_segment(v2_add(target, v2(0.0f, -20.0f)), v2_add(target, v2(0.0f, 20.0f)), cr, cg, cb, pulse * 0.7f);
    }

    /* --- Batched asteroid rendering with frustum culling + LOD --- */
    /* Pass 1: filled bodies (single triangle batch) */
    sgl_begin_triangles();
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t* a = &g.world.asteroids[i];
        if (!a->active) continue;
        if (!on_screen(a->pos.x, a->pos.y, a->radius + 16.0f)) continue;

        float progress_ratio = asteroid_progress_ratio(a);
        float body_r, body_g, body_b;
        asteroid_body_color(a->tier, a->commodity, progress_ratio, &body_r, &body_g, &body_b);

        int base_segs = 18;
        switch (a->tier) {
            case ASTEROID_TIER_XXL: base_segs = 28; break;
            case ASTEROID_TIER_XL:  base_segs = 22; break;
            case ASTEROID_TIER_L:   base_segs = 18; break;
            case ASTEROID_TIER_M:   base_segs = 15; break;
            case ASTEROID_TIER_S:   base_segs = 12; break;
            default: break;
        }
        int segments = lod_segments(base_segs, a->radius);

        sgl_c4f(body_r, body_g, body_b, 1.0f);
        float step = TWO_PI_F / (float)segments;
        float a0 = a->rotation;
        float r0 = asteroid_profile(a, a0);
        float prev_x = a->pos.x + cosf(a0) * r0;
        float prev_y = a->pos.y + sinf(a0) * r0;
        for (int j = 1; j <= segments; j++) {
            float a1 = a->rotation + (float)j * step;
            float r1 = asteroid_profile(a, a1);
            float cx = a->pos.x + cosf(a1) * r1;
            float cy = a->pos.y + sinf(a1) * r1;
            sgl_v2f(a->pos.x, a->pos.y);
            sgl_v2f(prev_x, prev_y);
            sgl_v2f(cx, cy);
            prev_x = cx;
            prev_y = cy;
        }
    }
    sgl_end();

    /* Pass 2: outlines + decorations (per-asteroid, needs LINE_STRIP) */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t* a = &g.world.asteroids[i];
        if (!a->active) continue;
        if (!on_screen(a->pos.x, a->pos.y, a->radius + 16.0f)) continue;

        bool is_target = (i == LOCAL_PLAYER.hover_asteroid);
        bool ineffective = is_target && LOCAL_PLAYER.beam_ineffective;
        float progress_ratio = asteroid_progress_ratio(a);
        float body_r, body_g, body_b;
        asteroid_body_color(a->tier, a->commodity, progress_ratio, &body_r, &body_g, &body_b);

        int base_segs = 18;
        switch (a->tier) {
            case ASTEROID_TIER_XXL: base_segs = 28; break;
            case ASTEROID_TIER_XL:  base_segs = 22; break;
            case ASTEROID_TIER_L:   base_segs = 18; break;
            case ASTEROID_TIER_M:   base_segs = 15; break;
            case ASTEROID_TIER_S:   base_segs = 12; break;
            default: break;
        }
        int segments = lod_segments(base_segs, a->radius);

        float rim_r = is_target ? (ineffective ? 1.0f : 0.45f) : (body_r * 0.85f);
        float rim_g = is_target ? (ineffective ? 0.15f : 0.94f) : (body_g * 0.95f);
        float rim_b = is_target ? (ineffective ? 0.1f : 1.0f) : fminf(1.0f, body_b * 1.2f);
        float rim_a = is_target ? 1.0f : 0.8f;

        sgl_c4f(rim_r, rim_g, rim_b, rim_a);
        sgl_begin_line_strip();
        for (int j = 0; j <= segments; j++) {
            float angle = a->rotation + ((float)j / (float)segments) * TWO_PI_F;
            float radius = asteroid_profile(a, angle);
            sgl_v2f(a->pos.x + cosf(angle) * radius, a->pos.y + sinf(angle) * radius);
        }
        sgl_end();

        /* Glow core for small/medium asteroids */
        if (a->tier == ASTEROID_TIER_S) {
            float cr, cg, cb;
            commodity_material_tint(a->commodity, &cr, &cg, &cb);
            draw_circle_filled(a->pos, a->radius * lerpf(0.14f, 0.24f, progress_ratio), 10,
                lerpf(0.48f, cr * 1.6f, 0.5f), lerpf(0.96f, cg * 1.6f, 0.5f),
                lerpf(0.78f, cb * 1.6f, 0.5f), lerpf(0.35f, 0.8f, progress_ratio));
        } else if (a->tier == ASTEROID_TIER_M) {
            float cr, cg, cb;
            commodity_material_tint(a->commodity, &cr, &cg, &cb);
            draw_circle_filled(a->pos, a->radius * 0.16f, 8,
                lerpf(0.36f, cr * 1.4f, 0.4f), lerpf(0.78f, cg * 1.4f, 0.4f),
                lerpf(0.98f, cb * 1.4f, 0.4f), 0.4f);
        }

        if (is_target && ineffective) {
            draw_circle_outline(a->pos, a->radius + 12.0f, 24, 1.0f, 0.2f, 0.15f, 0.75f);
        } else if (is_target) {
            draw_circle_outline(a->pos, a->radius + 12.0f, 24, 0.35f, 1.0f, 0.92f, 0.75f);
        }
    }
    draw_beam();
    draw_ship_tractor_field();
    draw_towed_tethers();
    draw_ship();
    draw_npc_ships();
    draw_remote_players(); /* Multiplayer: remote player ships */

    /* Ring trusses and modules render ON TOP of ships */
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t* st = &g.world.stations[i];
        if (!station_exists(st)) continue;
        if (!on_screen(st->pos.x, st->pos.y, st->dock_radius + 40.0f)) continue;
        bool is_current = LOCAL_PLAYER.docked && (i == LOCAL_PLAYER.current_station);
        bool is_nearby = (!LOCAL_PLAYER.docked) && (i == LOCAL_PLAYER.nearby_station);
        draw_station_rings(st, is_current, is_nearby);
    }
    draw_hopper_tractors();

    /* Module target highlight + info panel */
    if (g.target_station >= 0 && g.target_module >= 0) {
        const station_t *tst = &g.world.stations[g.target_station];
        if (g.target_module < tst->module_count) {
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
            const char *mod_names[] = {
                "DOCK", "ORE HOPPER", "FURNACE (FE)", "FURNACE (CU)", "FURNACE (CR)",
                "INGOT SELLER", "REPAIR BAY", "SIGNAL RELAY", "FRAME PRESS",
                "LASER FAB", "TRACTOR FAB", "CONTRACTS", "ORE SILO",
                "BLUEPRINTS", "RING", "SHIPYARD"
            };
            const char *name = (tm->type < MODULE_COUNT) ? mod_names[tm->type] : "MODULE";
            sdtx_puts(name);
            /* Module-specific info line */
            sdtx_color3b(180, 190, 210);
            sdtx_pos((sx + 60.0f) / cell, (sy - 8.0f) / cell);
            /* Module-specific info + action hint */
            commodity_t sell_c = -1;
            switch (tm->type) {
                case MODULE_FURNACE:     sell_c = COMMODITY_FERRITE_INGOT; break;
                case MODULE_FURNACE_CU:  sell_c = COMMODITY_CUPRITE_INGOT; break;
                case MODULE_FURNACE_CR:  sell_c = COMMODITY_CRYSTAL_INGOT; break;
                case MODULE_FRAME_PRESS: sell_c = COMMODITY_FRAME; break;
                case MODULE_LASER_FAB:   sell_c = COMMODITY_LASER_MODULE; break;
                case MODULE_TRACTOR_FAB: sell_c = COMMODITY_TRACTOR_MODULE; break;
                default: break;
            }
            if ((int)sell_c >= 0) {
                int stock = (int)lroundf(tst->inventory[sell_c]);
                int price = (int)lroundf(station_sell_price(tst, sell_c));
                if (stock > 0)
                    sdtx_printf("FIRE to buy  %d@%dcr  [%d]", stock, price, stock);
                else
                    sdtx_puts("Out of stock");
            } else switch (tm->type) {
                case MODULE_REPAIR_BAY:
                    sdtx_puts("Dock to repair hull");
                    break;
                case MODULE_ORE_BUYER: {
                    int ore = (int)lroundf(tst->inventory[COMMODITY_FERRITE_ORE] + tst->inventory[COMMODITY_CUPRITE_ORE] + tst->inventory[COMMODITY_CRYSTAL_ORE]);
                    sdtx_printf("Tow ore here  %d in stock", ore);
                    break;
                }
                case MODULE_DOCK:
                    sdtx_puts("FIRE to dock");
                    break;
                case MODULE_SIGNAL_RELAY:
                    sdtx_printf("Signal range %.0f", tst->signal_range);
                    break;
                default:
                    break;
            }
        }
    }

    /* Tracked contract target outline (yellow) */
    if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
        contract_t *ct = &g.world.contracts[g.tracked_contract];
        if (ct->active) {
            float pulse = 0.5f + 0.3f * sinf(g.world.time * 3.0f);
            if (ct->action == CONTRACT_DESTROY && ct->target_index >= 0 && ct->target_index < MAX_ASTEROIDS
                && g.world.asteroids[ct->target_index].active) {
                /* Outline the target asteroid */
                asteroid_t *a = &g.world.asteroids[ct->target_index];
                draw_circle_outline(a->pos, a->radius + 16.0f, 24, 1.0f, 0.87f, 0.20f, pulse);
            } else if (ct->action == CONTRACT_SUPPLY && ct->station_index < MAX_STATIONS) {
                /* Outline the target station */
                station_t *st = &g.world.stations[ct->station_index];
                if (station_exists(st))
                    draw_circle_outline(st->pos, st->dock_radius + 20.0f, 32, 1.0f, 0.87f, 0.20f, pulse);
            }
        }
    }
}

static void render_ui(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, screen_w, screen_h, 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    draw_hud_panels();
    draw_hud();

    /* Episode video popup — bottom-right corner, doesn't block gameplay */
    if (episode_is_active(&g.episode)) {
        episode_render(&g.episode, screen_w, screen_h);
    }

    /* Music track display — bottom-left, fades after 5s */
    if (g.music.playing && g.music.current_track >= 0) {
        float mt = g.music.track_display_timer;
        float music_alpha = 1.0f;
        if (mt < 0.5f) music_alpha = mt / 0.5f;              /* fade in */
        else if (mt > 5.0f) music_alpha = 1.0f - (mt - 5.0f) / 2.0f; /* fade out */
        if (g.music.paused) music_alpha = 1.0f;               /* always visible when paused */
        if (music_alpha > 0.01f) {
            const music_track_info_t *track = music_get_info(g.music.current_track);
            if (track) {
                sdtx_canvas(screen_w, screen_h);
                sdtx_origin(0.0f, 0.0f);
                float cell = 8.0f;
                float row = (screen_h - 16.0f) / cell;
                uint8_t a = (uint8_t)(music_alpha * 255.0f);
                /* Right-align: measure total width */
                char label[128];
                if (g.music.paused)
                    snprintf(label, sizeof(label), "PAUSED %s  [/] M", track->title);
                else
                    snprintf(label, sizeof(label), "%s  [/] M", track->title);
                float tw = (float)strlen(label) * cell;
                sdtx_pos((screen_w - tw - 12.0f) / cell, row);
                if (g.music.paused) {
                    sdtx_color4b(120, 100, 70, a);
                    sdtx_puts("PAUSED ");
                }
                sdtx_color4b(100, 90, 65, a);
                sdtx_puts(track->title);
                sdtx_color4b(60, 55, 45, a);
                sdtx_puts("  [/] M");
            }
        }
    }
}

/* interpolate_world_for_render: see net_sync.h/c */

static void render_frame(void) {
    interpolate_world_for_render();
    render_world();
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

static void frame(void) {
    float max_frame_dt = SIM_DT * (float)MAX_SIM_STEPS_PER_FRAME;
    float frame_dt = clampf((float)sapp_frame_duration(), 0.0f, max_frame_dt);

    /* --- Multiplayer: poll incoming and send input BEFORE sim --- */
    if (g.multiplayer_enabled) {
        bool was_connected = net_is_connected();
        net_poll();
        sync_local_player_slot_from_network();
        if (was_connected && !net_is_connected()) {
            set_notice("Connection lost. Continuing offline.");
            /* Fall back to local server using current world state */
            g.local_server.world = g.world;
            g.local_server.active = true;
        }
        /* Send input at ~30 Hz, or immediately if there's a one-shot action. */
        {
            uint8_t action = g.pending_net_action;
            g.net_input_timer -= frame_dt;
            if (g.net_input_timer <= 0.0f || action != 0) {
                g.net_input_timer = 1.0f / 30.0f;
                uint8_t flags = 0;
                if (g.input.key_down[SAPP_KEYCODE_W] || g.input.key_down[SAPP_KEYCODE_UP])
                    flags |= NET_INPUT_THRUST;
                if (g.input.key_down[SAPP_KEYCODE_S] || g.input.key_down[SAPP_KEYCODE_DOWN])
                    flags |= NET_INPUT_BRAKE;
                if (g.input.key_down[SAPP_KEYCODE_A] || g.input.key_down[SAPP_KEYCODE_LEFT])
                    flags |= NET_INPUT_LEFT;
                if (g.input.key_down[SAPP_KEYCODE_D] || g.input.key_down[SAPP_KEYCODE_RIGHT])
                    flags |= NET_INPUT_RIGHT;
                if (g.input.key_down[SAPP_KEYCODE_SPACE])
                    flags |= NET_INPUT_FIRE;
                g.pending_net_action = 0;
                uint8_t mining_target = (LOCAL_PLAYER.hover_asteroid >= 0 && LOCAL_PLAYER.hover_asteroid < 255)
                    ? (uint8_t)LOCAL_PLAYER.hover_asteroid : 255;
                net_send_input(flags, action, mining_target);
            }
        }
    }

    advance_simulation_frame(frame_dt);
    audio_generate_stream(&g.audio);

    render_frame();
}

static void cleanup(void) {
    episode_shutdown(&g.episode);
    music_shutdown(&g.music);
    if (g.multiplayer_enabled) {
        net_shutdown();
    }
    saudio_shutdown();
    sdtx_shutdown();
    sgl_shutdown();
    sg_shutdown();
}

static void event(const sapp_event* event) {
    switch (event->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
            if ((event->key_code >= 0) && (event->key_code < KEY_COUNT)) {
                g.input.key_down[event->key_code] = true;
                if (!event->key_repeat) {
                    g.input.key_pressed[event->key_code] = true;
                }
            }
            if (event->key_code == SAPP_KEYCODE_ESCAPE) {
                sapp_request_quit();
            }
            break;

        case SAPP_EVENTTYPE_KEY_UP:
            if ((event->key_code >= 0) && (event->key_code < KEY_COUNT)) {
                g.input.key_down[event->key_code] = false;
            }
            break;

        case SAPP_EVENTTYPE_UNFOCUSED:
        case SAPP_EVENTTYPE_SUSPENDED:
        case SAPP_EVENTTYPE_ICONIFIED:
            clear_input_state();
            break;

        default:
            break;
    }
}

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
