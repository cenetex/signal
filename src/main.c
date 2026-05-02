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
#include "signal_model.h"
#include "input.h"
#include "net_sync.h"
#include "onboarding.h"
#include "station_voice.h"
#include "avatar.h"
#include "mining_client.h"
#include "base58.h"


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

/* ship_total_cargo, ship_cargo_amount, station_buy_price, station_inventory_amount: see commodity.h/c */

/* format_ingot_stock_line: see station_ui.c */
/* station_at ... navigation_station_ptr: see station_ui.c */
/* station_role_name, station_role_short_name: see station_ui.c */
/* build_station_ui_state, format_station_* helpers: see station_ui.c */
/* station_role_hub_label, station_role_market_title, station_role_fit_title: see station_ui.c */
/* station_role_color: see station_ui.c */
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
    g.pending_net_buy_grade = MINING_GRADE_COUNT; /* sentinel = any */
    g.pending_net_place_station = -1;
    g.pending_net_place_ring    = -1;
    g.pending_net_place_slot    = -1;
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
     * batch payload (total_cr, grade_counts, any_by_contract) is
     * preserved so the renderer has everything it needs. */
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
            for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
                g.sell_batch.grade_counts[gi] = 0;
        }
    }
}

/* No sync_globals_to_world — world_t is the source of truth in single player. */

/* sync_world_to_globals removed — everything reads from g.world directly */

/* ------------------------------------------------------------------ */
/* Contextual hail: pick a station-authored message based on player state */
/* ------------------------------------------------------------------ */

static bool check_hail_condition(hail_cond_t cond) {
    const ship_t *ship = &LOCAL_PLAYER.ship;
    switch (cond) {
    case HAIL_COND_NO_TOWED:
        return ship->towed_count == 0;
    case HAIL_COND_HAS_TOWED:
        return ship->towed_count > 0;
    case HAIL_COND_LOW_CREDITS: {
        if (player_current_balance() >= 50.0f) return false;
        for (int s = 3; s < MAX_STATIONS; s++)
            if (station_exists(&g.world.stations[s])) return false;
        return true;
    }
    case HAIL_COND_HAS_CREDITS_NO_OUTPOST: {
        if (player_current_balance() < 200.0f) return false;
        for (int s = 3; s < MAX_STATIONS; s++)
            if (station_exists(&g.world.stations[s])) return false;
        return true;
    }
    case HAIL_COND_HAS_OUTPOST_NO_FURNACE:
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_is_active(&g.world.stations[s])) continue;
            if (!station_has_module(&g.world.stations[s], MODULE_FURNACE)) return true;
        }
        return false;
    case HAIL_COND_HAS_FURNACE:
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_is_active(&g.world.stations[s])) continue;
            if (station_has_module(&g.world.stations[s], MODULE_FURNACE)) return true;
        }
        return false;
    case HAIL_COND_HAS_NO_FRAMES:
        return ship->cargo[COMMODITY_FRAME] < 0.5f;
    case HAIL_COND_HAS_NO_SCAFFOLD:
        return ship->towed_scaffold < 0;
    case HAIL_COND_HAS_OUTPOST_NO_PRESS:
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_is_active(&g.world.stations[s])) continue;
            if (!station_has_module(&g.world.stations[s], MODULE_FRAME_PRESS)) return true;
        }
        return false;
    case HAIL_COND_HAS_PRESS:
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_is_active(&g.world.stations[s])) continue;
            if (station_has_module(&g.world.stations[s], MODULE_FRAME_PRESS)) return true;
        }
        return false;
    case HAIL_COND_NEVER_UPGRADED:
        return ship->mining_level == 0 && ship->hold_level == 0
            && ship->tractor_level == 0;
    case HAIL_COND_NO_SPECIALTY_FURNACE:
        /* True when at least one outpost station hasn't yet built up to
         * the cuprite-capable furnace tier (≥2 furnaces). The legacy
         * specialised CU/CR furnace types collapsed into MODULE_FURNACE
         * at the count-tier rework — a "specialty" stack is now defined
         * as 2+ furnaces. */
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_is_active(&g.world.stations[s])) continue;
            if (station_furnace_count(&g.world.stations[s]) < 2) return true;
        }
        return false;
    case HAIL_COND_ONE_OUTPOST: {
        int count = 0;
        for (int s = 3; s < MAX_STATIONS; s++)
            if (station_is_active(&g.world.stations[s])) count++;
        return count == 1;
    }
    case HAIL_COND_NEAR_EDGE: {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        return sig > 0.0f && sig < SIGNAL_BAND_FRINGE;
    }
    case HAIL_COND_DEFAULT:
        return true;
    default:
        return false;
    }
}

const char *contextual_hail_message(int station_index) {
    if (station_index < 0 || station_index >= 3) return NULL;
    const hail_response_t *table = STATION_HAIL_TABLES[station_index];
    int count = STATION_HAIL_COUNTS[station_index];
    for (int i = 0; i < count; i++) {
        if (check_hail_condition(table[i].condition))
            return table[i].message;
    }
    return NULL;
}

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
    onboarding_mark_fractured(); /* "mine" milestone = fired laser */
}

static void sim_on_dock(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_dock(&g.audio);
    g.screen_shake = fmaxf(g.screen_shake, 3.0f); /* dock clunk */
    g.dock_settle_timer = 1.0f; /* show ship settling before panel */
    int ds = LOCAL_PLAYER.current_station;
    if (ds < 3) {
        g.episode.stations_visited |= (1 << ds);
        if (g.episode.stations_visited == 7) /* all 3 */
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
        g.sell_batch.display_timer = 0.0f;
    }
    int grade_idx = (int)ev->sell.grade;
    if (grade_idx >= 0 && grade_idx < MINING_GRADE_COUNT)
        g.sell_batch.grade_counts[grade_idx]++;
    g.sell_batch.total_cr += total;
    if (ev->sell.by_contract) g.sell_batch.any_by_contract = true;
    g.sell_batch.active = true;
    g.sell_batch.settle_timer = 0.6f;
}

static void sim_on_sell(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    audio_play_sale(&g.audio);
    episode_trigger(&g.episode, 2); /* Ep 2: Furnace — first smelt */
    mining_client_record_strike((mining_grade_t)ev->sell.grade, ev->sell.bonus_cr);
    int total = ev->sell.base_cr + ev->sell.bonus_cr;
    if (total > 0) sell_batch_accumulate(ev, total);
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
}

static void sim_on_contract_complete(const sim_event_t *ev) {
    if (ev->contract_complete.action == CONTRACT_TRACTOR) {
        set_notice("Tractor contract fulfilled.");
        episode_trigger(&g.episode, 6); /* Ep 6: Hauler */
    } else if (ev->contract_complete.action == CONTRACT_FRACTURE) {
        set_notice("Fracture contract complete.");
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
    g.death_cinematic.active = true;
    g.death_cinematic.phase = 0;
    g.death_cinematic.pos = v2(ev->death.pos_x, ev->death.pos_y);
    g.death_cinematic.vel = v2(ev->death.vel_x, ev->death.vel_y);
    g.death_cinematic.angle = ev->death.angle;
    g.death_cinematic.spin = (((float)rand() / (float)RAND_MAX) - 0.5f) * 3.0f;
    g.death_cinematic.age = 0.0f;
    g.death_cinematic.menu_alpha = 0.0f;
    for (int s = 0; s < 8; s++) {
        float ang = ((float)s / 8.0f) * 2.0f * PI_F + (float)(s * 13 % 7) * 0.15f;
        float speed = 30.0f + (float)((s * 7 + 3) % 5) * 12.0f;
        g.death_cinematic.fragments[s][0] = 0.0f;
        g.death_cinematic.fragments[s][1] = 0.0f;
        g.death_cinematic.fragments[s][2] = cosf(ang) * speed + ev->death.vel_x * 0.6f;
        g.death_cinematic.fragments[s][3] = sinf(ang) * speed + ev->death.vel_y * 0.6f;
        g.death_cinematic.fragments[s][4] = ang;
        g.death_cinematic.fragments[s][5] = ((float)((s * 19 + 7) % 11) - 5.0f) * 0.6f;
    }
}

static void sim_on_death(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
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

/* Pick the hail message: contextual > avatar MOTD > station hardcoded. */
static const char *hail_choose_message(int station_idx) {
    const char *ctx = contextual_hail_message(station_idx);
    if (ctx) return ctx;
    const avatar_cache_t *av = avatar_get(station_idx);
    if (av && av->motd_fetched && av->motd[0]) return av->motd;
    return g.world.stations[station_idx].hail_message;
}

static void sim_on_hail_response(const sim_event_t *ev) {
    if (!ev_is_local(ev)) return;
    int hs = ev->hail_response.station;
    if (hs < 0 || hs >= MAX_STATIONS) return;

    snprintf(g.hail_station, sizeof(g.hail_station), "%s",
             g.world.stations[hs].name);
    snprintf(g.hail_message, sizeof(g.hail_message), "%s",
             hail_choose_message(hs));
    g.hail_credits = ev->hail_response.credits;
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
    set_notice("%s: %s  (balance %d %s)",
               g.hail_station, g.hail_message,
               (int)lroundf(ev->hail_response.credits), unit);
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
        ev->npc_spawned.home_station >= 3)
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
     * Run the same spoke + drag dynamics as the server. As long as
     * client and server start from matching arm_rotation/arm_omega
     * (snapshotted via station_authority sync) and tick at matching
     * dt, the rings stay coherent. */
    if (g.multiplayer_enabled) {
        g.world.time += dt;
        step_station_ring_dynamics(&g.world, dt);
    }

    /* Commission flash countdown */
    if (g.commission_timer > 0.0f)
        g.commission_timer = fmaxf(0.0f, g.commission_timer - dt);
    if (g.hail_timer > 0.0f)
        g.hail_timer = fmaxf(0.0f, g.hail_timer - dt);
    if (g.hail_ping_timer > 0.0f) {
        g.hail_ping_timer += dt;
        if (g.hail_ping_timer > 8.00f) g.hail_ping_timer = 0.0f; /* HAIL_PING_LIFECYCLE */
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

    /* Reset the docked view to DOCK on each fresh dock. */
    if (LOCAL_PLAYER.docked && !g.was_docked) {
        const station_t* st = &g.world.stations[LOCAL_PLAYER.current_station];
        g.station_view = STATION_VIEW_DOCK;
        g.selected_contract = -1; /* fresh dock — no carryover selection */
        /* Clear blueprint pip if we docked at the blueprint station */
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
        if (ds < 3) {
            g.episode.stations_visited |= (1 << ds);
            if (g.episode.stations_visited == 7)
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
    g.pass_action.colors[0].clear_value = (sg_color){ 0.018f, 0.024f, 0.045f, 1.0f };

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
            cbs.on_events = apply_remote_events;
            cbs.on_signal_channel = apply_remote_signal_channel;
            cbs.on_station_manifest = apply_remote_station_manifest;
            cbs.on_player_manifest = apply_remote_player_manifest;
            cbs.on_highscores = apply_remote_highscores;
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
                 * multiplayer was available. */
                g.local_server.active = false;
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

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(cam_left(), cam_right(), cam_top(), cam_bottom(), -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    draw_background(camera);
    draw_signal_borders();

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

        sgl_c4f(body_r, body_g, body_b, 1.0f);
        if (a->commodity == COMMODITY_CRYSTAL_ORE) {
            /* Crystals are constructed from explicit rectangles — no
             * polar profile, no segment smoothing. */
            draw_crystal_asteroid_fill(a);
            continue;
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

        float rim_r = is_target ? (ineffective ? 1.0f : 0.45f) : (body_r * 0.85f);
        float rim_g = is_target ? (ineffective ? 0.15f : 0.94f) : (body_g * 0.95f);
        float rim_b = is_target ? (ineffective ? 0.1f : 1.0f) : fminf(1.0f, body_b * 1.2f);
        float rim_a = is_target ? 1.0f : 0.8f;

        if (a->commodity == COMMODITY_CRYSTAL_ORE) {
            draw_crystal_asteroid_outline(a, rim_r, rim_g, rim_b, rim_a);
        } else {
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
            sgl_c4f(rim_r, rim_g, rim_b, rim_a);
            sgl_begin_line_strip();
            for (int j = 0; j <= segments; j++) {
                float angle = a->rotation + ((float)j / (float)segments) * TWO_PI_F;
                float radius = asteroid_profile(a, angle);
                sgl_v2f(a->pos.x + cosf(angle) * radius, a->pos.y + sinf(angle) * radius);
            }
            sgl_end();
        }

        /* Glow core (the "dot"). Common fragments keep the original
         * muted commodity tint — the belt should look mostly the
         * same. Fine+ rocks get the grade tint with a halo + pulse so
         * a strike actually catches the eye against the sea of dim
         * dots. M-tier always uses commodity tint (no payable ore). */
        if (a->tier == ASTEROID_TIER_S) {
            if (a->grade == 0) {
                /* Common: original commodity-tinted glow, untouched. */
                float cr, cg, cb;
                commodity_material_tint(a->commodity, &cr, &cg, &cb);
                draw_circle_filled(a->pos, a->radius * lerpf(0.14f, 0.24f, progress_ratio), 10,
                    lerpf(0.48f, cr * 1.6f, 0.5f), lerpf(0.96f, cg * 1.6f, 0.5f),
                    lerpf(0.78f, cb * 1.6f, 0.5f), lerpf(0.35f, 0.8f, progress_ratio));
            } else {
                /* Graded: special tint, bloom + halo scale with grade. */
                float cr, cg, cb;
                grade_tint(a->grade, &cr, &cg, &cb);
                float bloom = 1.10f + 0.18f * (float)(a->grade - 1);
                float pulse = (a->grade >= 3)
                    ? (1.0f + 0.18f * sinf(g.world.time * 6.0f))
                    : 1.0f;
                float base_r = a->radius * lerpf(0.18f, 0.30f, progress_ratio) * bloom * pulse;
                draw_circle_filled(a->pos, base_r, 12,
                    cr, cg, cb, lerpf(0.65f, 0.95f, progress_ratio));
                if (a->grade >= 2) {
                    draw_circle_outline(a->pos, base_r * 1.9f, 18,
                        cr, cg, cb, 0.45f * pulse);
                }
            }
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
                case MODULE_FURNACE: {
                    /* Headline ingot follows the count tier: 3+ → crystal,
                     * 2 → cuprite, 1 → ferrite. */
                    int n = station_furnace_count(&g.world.stations[LOCAL_PLAYER.current_station]);
                    if (n >= 3)      sell_c = COMMODITY_CRYSTAL_INGOT;
                    else if (n == 2) sell_c = COMMODITY_CUPRITE_INGOT;
                    else             sell_c = COMMODITY_FERRITE_INGOT;
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
                    sdtx_printf("[Fire] buy 1 @ %dcr  (stock %d)", price, stock);
                else
                    sdtx_puts("Out of stock");
            } else switch (tm->type) {
                case MODULE_REPAIR_BAY:
                    sdtx_puts("Dock to repair hull");
                    break;
                case MODULE_DOCK:
                    sdtx_puts("[Fire] dock");
                    break;
                case MODULE_SIGNAL_RELAY:
                    sdtx_printf("Signal range %.0f", tst->signal_range);
                    break;
                default:
                    break;
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

static void render_frame(void) {
    interpolate_world_for_render();

    /* Damage vignette back wave — sgl-queued before world geometry so
     * world content draws on top. Front wave is queued later by the HUD
     * pass. Set screen-space ortho explicitly; render_world will overwrite
     * with its world-space matrices. */
    {
        float screen_w = ui_screen_width();
        float screen_h = ui_screen_height();
        sgl_matrix_mode_projection();
        sgl_load_identity();
        sgl_ortho(0.0f, screen_w, screen_h, 0.0f, -1.0f, 1.0f);
        sgl_matrix_mode_modelview();
        sgl_load_identity();
        draw_hull_fog_back();
    }

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
            set_notice("Connection lost. Reload to reconnect.");
            /* Fall back to local server using current world state */
            g.local_server.world = g.world;
            g.local_server.active = true;
        }
        /* P key: reconnect to server */
        if (!net_is_connected() && g.local_server.active &&
            is_key_pressed(SAPP_KEYCODE_P)) {
            if (net_reconnect()) {
                set_notice("Reconnecting...");
                g.local_server.active = false;
            }
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
                if (g.input.key_down[SAPP_KEYCODE_M])
                    flags |= NET_INPUT_FIRE;
                if (g.input.key_down[SAPP_KEYCODE_SPACE] && !g.plan_mode_active)
                    flags |= NET_INPUT_TRACTOR;
                uint8_t buy_grade_byte = g.pending_net_buy_grade;
                int8_t place_station = g.pending_net_place_station;
                int8_t place_ring    = g.pending_net_place_ring;
                int8_t place_slot    = g.pending_net_place_slot;
                g.pending_net_action = 0;
                g.pending_net_buy_grade = MINING_GRADE_COUNT;
                g.pending_net_place_station = -1;
                g.pending_net_place_ring    = -1;
                g.pending_net_place_slot    = -1;
                uint8_t mining_target = (LOCAL_PLAYER.hover_asteroid >= 0 && LOCAL_PLAYER.hover_asteroid < 255)
                    ? (uint8_t)LOCAL_PLAYER.hover_asteroid : 255;
                /* Layer A.3 of #479 — migrate state-changing actions
                 * onto the signed channel when an identity secret is
                 * available. Transient input (movement, mining-beam-on)
                 * stays on the unsigned NET_MSG_INPUT channel: signing
                 * every 60Hz frame would saturate the server. We send
                 * the signed action FIRST, then clear `action` so the
                 * unsigned send below carries only transient bits.
                 *
                 * Pre-A.3 clients (no identity secret) keep using the
                 * unsigned action path; the server still accepts both
                 * for the deprecation window. */
                if (action >= NET_ACTION_BUY_PRODUCT &&
                    action < NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT &&
                    net_has_identity_secret()) {
                    uint8_t payload[2] = {
                        (uint8_t)(action - NET_ACTION_BUY_PRODUCT),
                        buy_grade_byte
                    };
                    if (net_send_signed_action(SIGNED_ACTION_BUY_PRODUCT,
                                               payload, sizeof(payload))) {
                        action = NET_ACTION_NONE;
                    }
                }
                net_send_input(flags, action, mining_target, buy_grade_byte,
                               place_station, place_ring, place_slot);
            }
        }
    }

    advance_simulation_frame(frame_dt);

    /* Phase 2: keep the client-side manifest summary fresh. In SP this
     * reads the local manifest; in MP it's a no-op relative to the net
     * path which fills the summary directly (see TODO in src/net.c). */
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
