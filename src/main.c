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
#include "avatar.h"

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
    g.selected_contract = -1;
    g.target_station = -1;
    g.target_module = -1;
    g.inspect_station = -1;
    g.inspect_module = -1;
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
    /* Beam state is server-authoritative in multiplayer — don't clear it
     * here or we wipe what apply_remote_player_state just set before
     * render gets a chance to draw it. In singleplayer the local server
     * mirror overwrites beam state every tick anyway. */
    if (!g.multiplayer_enabled) {
        LOCAL_PLAYER.beam_active = false;
        LOCAL_PLAYER.beam_hit = false;
    }
    g.thrusting = false;
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
                    /* Start music on first launch — shuffled */
                    if (!g.music.playing && !g.music.loading) {
                        music_next_track(&g.music);
                    }
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
                if (ev->player_id == g.local_player_slot) {
                    audio_play_damage(&g.audio, ev->damage.amount);
                    /* Screen shake scales with damage. Tunables chosen so a
                     * minor scrape (~2 hp) wiggles a few pixels and a
                     * full ramming hit (~30 hp) noticeably jolts. */
                    float kick = sqrtf(ev->damage.amount) * 4.0f;
                    if (kick > 40.0f) kick = 40.0f;
                    if (kick > g.screen_shake) g.screen_shake = kick;
                }
                break;
            case SIM_EVENT_CONTRACT_COMPLETE:
                if (ev->contract_complete.action == CONTRACT_TRACTOR) {
                    set_notice("Tractor contract fulfilled.");
                    episode_trigger(&g.episode, 6); /* Ep 6: Hauler */
                } else if (ev->contract_complete.action == CONTRACT_FRACTURE) {
                    set_notice("Fracture contract complete.");
                }
                break;
            case SIM_EVENT_SCAFFOLD_READY: {
                int sidx = ev->scaffold_ready.station;
                int mtype = ev->scaffold_ready.module_type;
                if (sidx >= 0 && sidx < MAX_STATIONS) {
                    set_notice("%s scaffold ready at %s.",
                        module_type_name((module_type_t)mtype),
                        g.world.stations[sidx].name);
                }
                break;
            }
            case SIM_EVENT_OUTPOST_PLACED: {
                /* Transition from ghost preview to real plan mode:
                 * the server just created the planned station at the
                 * position where the player locked it. */
                if (ev->player_id == g.local_player_slot) {
                    g.plan_target_station = ev->outpost_placed.slot;
                    g.placement_target_station = ev->outpost_placed.slot;
                }
                break;
            }
            case SIM_EVENT_DEATH:
                if (ev->player_id == g.local_player_slot) {
                    g.death_ore_mined = ev->death.ore_mined;
                    g.death_credits_earned = ev->death.credits_earned;
                    g.death_credits_spent = ev->death.credits_spent;
                    g.death_asteroids_fractured = ev->death.asteroids_fractured;
                    /* Snapshot the wreckage at the death position. The
                     * server has already respawned the ship at a station,
                     * so we use the position from the death event payload
                     * (captured before the server moved the hull). */
                    g.death_cinematic.active = true;
                    g.death_cinematic.phase = 0;
                    g.death_cinematic.pos = v2(ev->death.pos_x, ev->death.pos_y);
                    g.death_cinematic.vel = v2(ev->death.vel_x, ev->death.vel_y);
                    g.death_cinematic.angle = ev->death.angle;
                    g.death_cinematic.spin = (((float)rand() / (float)RAND_MAX) - 0.5f) * 3.0f;
                    g.death_cinematic.age = 0.0f;
                    g.death_cinematic.menu_alpha = 0.0f;
                    /* 8 shards radiating outward from the wreck. Each shard
                     * starts at the ship origin with a randomized velocity
                     * and spin. Components: [dx, dy, vx, vy, ang, spin]. */
                    for (int i = 0; i < 8; i++) {
                        float ang = ((float)i / 8.0f) * 2.0f * PI_F + (float)(i * 13 % 7) * 0.15f;
                        float speed = 30.0f + (float)((i * 7 + 3) % 5) * 12.0f;
                        g.death_cinematic.fragments[i][0] = 0.0f;
                        g.death_cinematic.fragments[i][1] = 0.0f;
                        g.death_cinematic.fragments[i][2] = cosf(ang) * speed + ev->death.vel_x * 0.6f;
                        g.death_cinematic.fragments[i][3] = sinf(ang) * speed + ev->death.vel_y * 0.6f;
                        g.death_cinematic.fragments[i][4] = ang;
                        g.death_cinematic.fragments[i][5] = ((float)((i * 19 + 7) % 11) - 5.0f) * 0.6f;
                    }
                    /* Legacy timer kept for the auto-fade fallback path
                     * but cinematic logic ignores it. */
                    g.death_screen_timer = 0.0f;
                    g.death_screen_max = 0.0f;
                    episode_trigger(&g.episode, 9); /* Ep 9: Death */
                    memset(g.episode.watched, 0, sizeof(g.episode.watched));
                    g.episode.stations_visited = 0;
                    episode_save(&g.episode);
                }
                break;
            case SIM_EVENT_HAIL_RESPONSE:
                if (ev->player_id == g.local_player_slot) {
                    int hs = ev->hail_response.station;
                    if (hs >= 0 && hs < MAX_STATIONS) {
                        snprintf(g.hail_station, sizeof(g.hail_station), "%s", g.world.stations[hs].name);
                        /* Use CDN MOTD if fetched, otherwise fall back to hardcoded */
                        const avatar_cache_t *av = avatar_get(hs);
                        if (av && av->motd_fetched && av->motd[0])
                            snprintf(g.hail_message, sizeof(g.hail_message), "%s", av->motd);
                        else
                            snprintf(g.hail_message, sizeof(g.hail_message), "%s", g.world.stations[hs].hail_message);
                        g.hail_credits = ev->hail_response.credits;
                        g.hail_station_index = hs;
                        g.hail_timer = 6.0f;
                        if (g.hail_credits > 0.5f)
                            audio_play_sale(&g.audio);
                        /* Fetch portrait if station has a slug */
                        if (g.world.stations[hs].station_slug[0])
                            avatar_fetch(hs, g.world.stations[hs].station_slug);
                    }
                }
                break;
            case SIM_EVENT_MODULE_ACTIVATED: {
                int si = ev->module_activated.station;
                int mi = ev->module_activated.module_idx;
                station_t *act_st = &g.world.stations[si];
                vec2 mpos = module_world_pos_ring(act_st,
                    act_st->modules[mi].ring, act_st->modules[mi].slot);
                g.commission_timer = 1.5f;
                g.commission_pos = mpos;
                module_color_fn((module_type_t)ev->module_activated.module_type,
                    &g.commission_cr, &g.commission_cg, &g.commission_cb);
                audio_play_commission(&g.audio);
                set_notice("%s online.", module_type_name((module_type_t)ev->module_activated.module_type));
                break;
            }
            case SIM_EVENT_OUTPOST_ACTIVATED:
                if (!g.episode.watched[4])
                    episode_trigger(&g.episode, 4); /* Ep 4: Naming */
                audio_play_commission(&g.audio);
                break;
            case SIM_EVENT_NPC_SPAWNED:
                /* Ep 5: Drones — first miner at a player outpost */
                if (!g.episode.watched[5] &&
                    ev->npc_spawned.role == NPC_ROLE_MINER &&
                    ev->npc_spawned.home_station >= 3)
                    episode_trigger(&g.episode, 5);
                break;
            case SIM_EVENT_SIGNAL_LOST:
                if (ev->player_id == g.local_player_slot && !g.episode.watched[7])
                    episode_trigger(&g.episode, 7); /* Ep 7: Dark Sector */
                break;
            case SIM_EVENT_STATION_CONNECTED:
                if (!g.episode.watched[8] && ev->station_connected.connected_count >= 5)
                    episode_trigger(&g.episode, 8); /* Ep 8: Every AI Dreams */
                break;
            default:
                break;
        }
    }
}

static void onboarding_per_frame(void) {
    if (g.onboarding.complete) return;
    if (!g.onboarding.collected && LOCAL_PLAYER.ship.towed_count > 0)
        onboarding_mark_collected();
    if (!g.onboarding.towed && g.onboarding.collected && LOCAL_PLAYER.ship.towed_count == 0 &&
        LOCAL_PLAYER.nearby_station >= 0)
        onboarding_mark_towed();
    if (!g.onboarding.bought) {
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
            if (LOCAL_PLAYER.ship.cargo[c] > 0.5f) { onboarding_mark_bought(); break; }
    }
    if (!g.onboarding.got_scaffold && LOCAL_PLAYER.ship.towed_scaffold >= 0)
        onboarding_mark_got_scaffold();
    if (!g.onboarding.placed_outpost) {
        for (int s = 3; s < MAX_STATIONS; s++)
            if (station_exists(&g.world.stations[s])) { onboarding_mark_placed_outpost(); break; }
    }
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
     * Derive ring rotation from world time — deterministic from speed * time. */
    if (g.multiplayer_enabled) {
        g.world.time += dt;
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

    /* Commission flash countdown */
    if (g.commission_timer > 0.0f)
        g.commission_timer = fmaxf(0.0f, g.commission_timer - dt);
    if (g.hail_timer > 0.0f)
        g.hail_timer = fmaxf(0.0f, g.hail_timer - dt);
    if (g.outpost_lock_timer > 0.0f)
        g.outpost_lock_timer = fmaxf(0.0f, g.outpost_lock_timer - dt);

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
     *   Phase 0 (drift): wreckage tumbles, fog rolls in. After 2s the
     *      cinematic auto-advances to phase 1 — no input required.
     *   Phase 1 (stats): the stat menu fades in over the wreckage.
     *      Pressing E (after the menu has settled) releases the
     *      cinematic and lets the same E press fall through to the
     *      normal docked-launch handler.
     *   Phase 2 (closing): cinematic.active is false but menu_alpha
     *      decays back toward 0 so the stat screen visibly disappears. */
    if (g.death_cinematic.active) {
        if (g.death_cinematic.phase == 0 && g.death_cinematic.age >= 2.0f) {
            g.death_cinematic.phase = 1;
        }
        if (g.death_cinematic.phase == 1 && g.death_cinematic.menu_alpha >= 0.85f
            && is_key_pressed(SAPP_KEYCODE_E)) {
            g.death_cinematic.active = false;
            g.death_cinematic.phase = 2;
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

    /* Tab switching while docked */
    if (LOCAL_PLAYER.docked && !g.was_docked) {
        /* Just docked — reset to overview (or construction for scaffolds) */
        const station_t* st = &g.world.stations[LOCAL_PLAYER.current_station];
        g.station_tab = STATION_TAB_STATUS;
        g.selected_contract = -1; /* fresh dock — no carryover selection */
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
    if (LOCAL_PLAYER.docked && (is_key_pressed(SAPP_KEYCODE_TAB) || is_key_pressed(SAPP_KEYCODE_Q))) {
        station_tab_t vtabs[STATION_TAB_COUNT];
        int vtab_count = 0;
        vtabs[vtab_count++] = STATION_TAB_STATUS;
        vtabs[vtab_count++] = STATION_TAB_MARKET;
        vtabs[vtab_count++] = STATION_TAB_CONTRACTS;
        const station_t *cst = current_station_ptr();
        if (cst && station_has_module(cst, MODULE_SHIPYARD)) {
            vtabs[vtab_count++] = STATION_TAB_SHIPYARD;
        }
        int cur = 0;
        for (int i = 0; i < vtab_count; i++) { if (vtabs[i] == g.station_tab) { cur = i; break; } }
        int dir = is_key_pressed(SAPP_KEYCODE_TAB) ? 1 : (vtab_count - 1);
        g.station_tab = vtabs[(cur + dir) % vtab_count];
        g.selected_contract = -1; /* tab change clears stale selection */
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

    /* Advance interpolation timers (both modes) */
    g.asteroid_interp.t += dt / fmaxf(g.asteroid_interp.interval, 0.01f);
    g.npc_interp.t += dt / fmaxf(g.npc_interp.interval, 0.01f);
    g.player_interp.t += dt / fmaxf(g.player_interp.interval, 0.01f);

    /* Thrust flame: local input for manual, server input for autopilot.
     * In SP the local server mirrors input.thrust; in MP the PLAYER_STATE
     * flags carry bit0=thrust which we decode into g.server_thrusting. */
    if (LOCAL_PLAYER.autopilot_mode) {
        g.thrusting = g.server_thrusting && !LOCAL_PLAYER.docked;
    } else {
        g.thrusting = (intent.thrust > 0.0f) && !LOCAL_PLAYER.docked;
    }

    /* Play audio from sim events (singleplayer only — multiplayer has no server events) */
    process_sim_events(&g.world.events);

    /* Detect state transitions for music/episode triggers (works in both modes).
     * Must run BEFORE was_docked is updated to detect the transition. */
    if (g.was_docked && !LOCAL_PLAYER.docked) {
        /* Just launched */
        fprintf(stderr, "LAUNCH: triggering ep 0 + music\n");
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

    /* Death: handled by SIM_EVENT_DEATH (singleplayer) or NET_MSG_DEATH (multiplayer) */

    /* Update was_docked AFTER transition checks */
    g.was_docked = LOCAL_PLAYER.docked;

    onboarding_per_frame();
    episode_per_frame();
    episode_update(&g.episode, dt);
    music_update(&g.music, dt);

    /* X = self-destruct handled via input intent (works in both modes) */

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
    avatar_init();
    hull_fog_init();

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
            cbs.on_scaffolds = apply_remote_scaffolds;
            cbs.on_hail_response = apply_remote_hail_response;
            cbs.on_player_ship = apply_remote_player_ship;
            cbs.on_contracts = apply_remote_contracts;
            cbs.on_death = on_remote_death;
            cbs.on_world_time = on_remote_world_time;
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
    float half_w = sapp_widthf() * 0.5f;
    float half_h = sapp_heightf() * 0.5f;
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

        /* Smelting glow: fragment turns bright orange-white as it cooks */
        if (a->smelt_progress > 0.01f) {
            float sp = a->smelt_progress;
            body_r = body_r + (1.0f - body_r) * sp * 0.8f;
            body_g = body_g + (0.6f - body_g) * sp * 0.6f;
            body_b = body_b + (0.2f - body_b) * sp * 0.3f;
        }

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
    draw_compass_ring();   /* Navigation compass around player ship */

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
            sdtx_puts(module_type_name(tm->type));
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
            if (ct->action == CONTRACT_FRACTURE && ct->target_index >= 0 && ct->target_index < MAX_ASTEROIDS
                && g.world.asteroids[ct->target_index].active) {
                /* Outline the target asteroid */
                asteroid_t *a = &g.world.asteroids[ct->target_index];
                draw_circle_outline(a->pos, a->radius + 16.0f, 24, 1.0f, 0.87f, 0.20f, pulse);
            } else if (ct->action == CONTRACT_TRACTOR && ct->station_index < MAX_STATIONS) {
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
    avatar_shutdown();
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
