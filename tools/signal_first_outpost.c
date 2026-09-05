/* Fresh-world release route. The driver supplies player inputs each tick. */
#include "game_sim.h"
#include "sim_flight.h"
#include "sim_nav.h"
#include "pubkey_proof.h"
#include "protocol.h"
#include "net_protocol.h"
#include "signal_crypto.h"
#include "chain_log.h"
#include "persistence_generation.h"
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PHASE_LAUNCH, PHASE_SELECT_ROCK, PHASE_MINE, PHASE_COLLECT_ORE,
    PHASE_DELIVER_ORE, PHASE_DOCK, PHASE_REFIT, PHASE_BUY_INGOTS,
    PHASE_HAUL, PHASE_UNPACK_INGOTS, PHASE_DELIVER_INGOTS,
    PHASE_DELIVER_FRAMES, PHASE_COLLECT_FRAMES, PHASE_TOW_RELAY, PHASE_WAIT_FRAMES, PHASE_EXIT_FRAMES
} route_phase_t;

static bool authenticate(world_t *w, uint8_t pubkey[32], uint8_t secret[64]) {
    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, w);
    sp->session_ready = true;
    memset(sp->session_token, 1, 8);
    const uint8_t seed[32] = {6, 0, 0, 1};
    signal_crypto_keypair_from_seed(seed, pubkey, secret);
    uint8_t registration[REGISTER_PUBKEY_MSG_SIZE] = {NET_MSG_REGISTER_PUBKEY};
    memcpy(registration + 1, pubkey, 32);
    server_pubkey_register_result_t registered;
    if (!server_dispatch_register_pubkey_message(w, 0, registration,
            sizeof(registration), &registered)) return false;
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    uint8_t signature[SIGNAL_CRYPTO_SIG_BYTES];
    if (!server_issue_pubkey_challenge(w, 0, challenge) ||
        !pubkey_proof_sign(signature, pubkey, secret, sp->session_token,
                           challenge)) return false;
    uint8_t proof[PROVE_PUBKEY_MSG_SIZE] = {NET_MSG_PROVE_PUBKEY};
    memcpy(proof + PROVE_PUBKEY_PUBKEY_OFFSET, pubkey, 32);
    memcpy(proof + PROVE_PUBKEY_TOKEN_OFFSET, sp->session_token, 8);
    memcpy(proof + PROVE_PUBKEY_SIG_OFFSET, signature, sizeof(signature));
    server_pubkey_proof_result_t proved;
    return server_dispatch_pubkey_proof_message(w, 0, proof, sizeof(proof),
                                                &proved) && proved.verified &&
           server_finalize_pubkey_identity(w, 0);
}

static bool load_checkpoint(world_t *copy, const persistence_generation_paths_t *saved,
                            uint8_t pubkey[32], uint8_t secret[64]) {
    int catalogs = station_catalog_load_all(copy->stations, MAX_STATIONS, saved->catalog_dir);
    bool loaded = catalogs >= 0 && world_load(copy, saved->world_path);
    bool authenticated = loaded && authenticate(copy, pubkey, secret);
    bool restored = authenticated && player_load_by_pubkey(&copy->players[0], copy,
                                                           saved->player_dir, pubkey);
    fprintf(stderr, "checkpoint catalogs=%d world=%d authentication=%d player=%d\n",
            catalogs, loaded, authenticated, restored);
    return restored;
}

static bool checkpoint_roundtrip(world_t *w, const char *root,
                                 uint8_t pubkey[32], uint8_t secret[64]) {
    char saves[PERSISTENCE_GENERATION_PATH_MAX];
    int size = snprintf(saves, sizeof(saves), "%s/checkpoints", root);
    if (size <= 0 || (size_t)size >= sizeof(saves)) return false;
    bool slots[MAX_PLAYERS] = {true};
    persistence_generation_paths_t saved;
    if (!persistence_generation_commit(saves, NULL, w, slots,
            PERSISTENCE_GENERATION_FAULT_NONE, &saved)) return false;
    world_t *copy = calloc(1, sizeof(*copy));
    if (!copy) return false;
    bool ok = load_checkpoint(copy, &saved, pubkey, secret);
    const server_player_t *before = &w->players[0];
    const server_player_t *after = &copy->players[0];
    fprintf(stderr, "checkpoint decoded=%d\n", ok);
    if (ok) {
        fprintf(stderr, "checkpoint compare asset=%u/%u nonce=%llu/%llu mining=%d/%d hold=%d/%d manifest=%u/%u ore=%d/%d pods=%d/%d scaffold=%d/%d stations=%d/%d\n",
            before->ship_asset_id, after->ship_asset_id,
            (unsigned long long)before->last_signed_nonce, (unsigned long long)after->last_signed_nonce,
            before->ship->mining_level, after->ship->mining_level,
            before->ship->hold_level, after->ship->hold_level,
            before->ship->manifest.count, after->ship->manifest.count,
            before->ship->towed_count, after->ship->towed_count,
            before->ship->towed_pod_count, after->ship->towed_pod_count,
            before->ship->towed_scaffold, after->ship->towed_scaffold,
            w->station_count, copy->station_count);
        ok = before->ship_asset_id == after->ship_asset_id &&
             before->last_signed_nonce == after->last_signed_nonce &&
             before->ship->mining_level == after->ship->mining_level &&
             before->ship->hold_level == after->ship->hold_level &&
             before->ship->manifest.count == after->ship->manifest.count &&
             before->ship->towed_count == after->ship->towed_count &&
             before->ship->towed_pod_count == after->ship->towed_pod_count &&
             before->ship->towed_scaffold == after->ship->towed_scaffold &&
             w->station_count == copy->station_count;
        for (int i = 0; ok && i < w->station_count; i++) {
            ok = ledger_balance_by_pubkey(&w->stations[i], pubkey) ==
                 ledger_balance_by_pubkey(&copy->stations[i], pubkey) &&
                 w->stations[i].scaffold == copy->stations[i].scaffold &&
                 memcmp(w->stations[i].outpost_founder_pubkey,
                        copy->stations[i].outpost_founder_pubkey, 32) == 0;
        }
        for (int i = 0; ok && i < before->ship->towed_pod_count; i++) {
            int lhs_index = before->ship->towed_pods[i];
            int rhs_index = after->ship->towed_pods[i];
            uint8_t lhs[32], rhs[32];
            ok = lhs_index == rhs_index &&
                 cargo_pod_selection_digest(&w->cargo_pods[lhs_index], lhs) &&
                 cargo_pod_selection_digest(&copy->cargo_pods[rhs_index], rhs) &&
                 memcmp(lhs, rhs, sizeof(lhs)) == 0;
        }
        for (uint16_t i = 0; ok && i < before->ship->manifest.count; i++) {
            uint8_t lhs[CARGO_UNIT_WIRE_SIZE], rhs[CARGO_UNIT_WIRE_SIZE];
            cargo_unit_wire_pack(&before->ship->manifest.units[i], lhs);
            cargo_unit_wire_pack(&after->ship->manifest.units[i], rhs);
            ok = memcmp(lhs, rhs, sizeof(lhs)) == 0;
        }
    }
    fprintf(stderr, "checkpoint tick=%u generation=%llu restored=%d\n", w->tick,
            (unsigned long long)saved.generation, ok);
    world_cleanup(copy);
    free(copy);
    return ok;
}

static vec2 dock_route_target(const station_t *st, const ship_t *ship) {
    vec2 relative = v2_sub(ship->pos, st->pos);
    float radius = v2_len(relative);
    if (radius < DOCK_APPROACH_RANGE + 40)
        return station_approach_target(st, ship->pos);
    float angle = fixp_atan2f(relative.y, relative.x);
    if (radius > STATION_RING_RADIUS[station_max_ring(st)] + 200)
        return station_entry_target(st, ship->pos);
    for (int ring = station_max_ring(st); ring >= 1; ring--) {
        float inner = STATION_RING_RADIUS[ring] - 70;
        if (radius < inner || ring_module_count(st, ring) <= 1) continue;
        if (!station_ring_open_gap_lane(st, ring, NULL, NULL)) continue;
        float lane = station_ring_open_gap_angle(st, ring);
        float difference = wrap_angle(lane - angle);
        if (fabsf(difference) > 0.12f) {
            float waypoint_angle = angle + clampf(difference, -0.3f, 0.3f);
            float outside = fmaxf(radius, STATION_RING_RADIUS[ring] + 100);
            return v2_add(st->pos, v2_scale(v2_from_angle(waypoint_angle), outside));
        }
        return station_ring_open_gap_lane_pos(st, ring, inner);
    }
    return station_approach_target(st, ship->pos);
}

static vec2 exit_route_target(const station_t *st, const ship_t *ship) {
    vec2 relative = v2_sub(ship->pos, st->pos);
    float radius = v2_len(relative);
    float angle = fixp_atan2f(relative.y, relative.x);
    for (int ring = 1; ring <= station_max_ring(st); ring++) {
        if (radius > STATION_RING_RADIUS[ring] + 10 || ring_module_count(st, ring) <= 1)
            continue;
        if (!station_ring_open_gap_lane(st, ring, NULL, NULL)) continue;
        float lane = station_ring_open_gap_angle(st, ring);
        float difference = wrap_angle(lane - angle);
        if (fabsf(difference) > 0.25f) {
            float waypoint_angle = angle + clampf(difference, -0.3f, 0.3f);
            float inside = fminf(radius, STATION_RING_RADIUS[ring] - 70);
            return v2_add(st->pos, v2_scale(v2_from_angle(waypoint_angle), inside));
        }
        return station_ring_open_gap_lane_pos(st, ring, STATION_RING_RADIUS[ring] + 70);
    }
    return v2_add(st->pos, v2_scale(v2_from_angle(angle), 1700));
}

static int nearest_rock(const world_t *w, const ship_t *ship, bool fragment, int home) {
    int best = -1;
    float distance = 1e30f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->commodity != COMMODITY_FERRITE_ORE ||
            asteroid_is_collectible(a) != fragment ||
            (!fragment && !mining_level_can_fracture_asteroid(ship->mining_level, a)))
            continue;
        if (!fragment && v2_dist_sq(a->pos, w->stations[home].pos) > 4000.0f * 4000.0f) continue;
        float d = v2_dist_sq(ship->pos, a->pos);
        if (d < distance) { best = i; distance = d; }
    }
    return best;
}

static vec2 drop_point(const station_t *st) {
    float best = 1e30f;
    vec2 target = st->pos;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *f = &st->modules[i];
        if (f->scaffold || f->type != MODULE_FURNACE ||
            module_instance_input_ore(f) != COMMODITY_FERRITE_ORE) continue;
        vec2 fp = module_world_pos_ring(st, f->ring, f->slot);
        for (int j = 0; j < st->module_count; j++) {
            const station_module_t *h = &st->modules[j];
            if (h->scaffold || h->type != MODULE_HOPPER ||
                h->commodity != COMMODITY_FERRITE_ORE || abs(h->ring - f->ring) != 1)
                continue;
            vec2 hp = module_world_pos_ring(st, h->ring, h->slot);
            float d = v2_dist_sq(fp, hp);
            if (d < best) { best = d; target = v2_scale(v2_add(fp, hp), 0.5f); }
        }
    }
    return target;
}

static flight_cmd_t steer_through_lane(const ship_t *ship, vec2 target) {
    vec2 delta = v2_sub(target, ship->pos);
    float distance = v2_len(delta);
    vec2 wanted = distance > 1 ? v2_scale(delta, fminf(50, distance) / distance) : v2(0, 0);
    vec2 error = v2_sub(wanted, ship->vel);
    float magnitude = v2_len(error);
    if (magnitude < 0.5f) return (flight_cmd_t){0};
    float heading = fixp_atan2f(error.y, error.x);
    float facing = fixp_cosf(wrap_angle(heading - ship->angle));
    return (flight_cmd_t){
        .turn = flight_face_heading(ship, heading),
        .thrust = facing > 0.7f ? fminf(0.3f, magnitude / 100.0f) : 0.0f,
    };
}

static void apply_flight(input_intent_t *input, flight_cmd_t command) {
    input->turn = command.turn;
    input->thrust = command.thrust;
    input->reverse_thrust = command.reverse_thrust;
}

static int affordable_pod(const world_t *w, const server_player_t *sp,
                          int station, commodity_t commodity) {
    float balance = ledger_balance_by_pubkey(&w->stations[station], sp->pubkey);
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        int owner = -1, module = -1;
        if (!pod->active || pod->commodity != commodity ||
            !cargo_pod_module_tractor_indices(pod, &owner, &module) ||
            owner != station || module < 0 ||
            w->stations[station].modules[module].type != MODULE_DOCK) continue;
        float cost = station_market_pod_sell_quote(&w->stations[station], pod);
        if (cost > 0 && balance >= cost) return i;
    }
    return -1;
}

static bool has_undelivered_ingots(const world_t *w, const ship_t *ship, int station) {
    for (int i = 0; i < ship->towed_pod_count; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[ship->towed_pods[i]];
        if (pod->active && pod->commodity == COMMODITY_FERRITE_INGOT &&
            cargo_pod_custody_station(pod) != station) return true;
    }
    return false;
}

static bool pod_origin_is(const cargo_pod_t *pod, int station) {
    if (!pod->manifest_count) return false;
    for (uint16_t i = 0; i < pod->manifest_count; i++)
        if (pod->manifest_units[i].origin_station != station) return false;
    return true;
}

static int local_frame_pod(const world_t *w, const ship_t *ship, bool towed_only) {
    int best = -1;
    float distance = 1e30f;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->commodity != COMMODITY_FRAME || !pod_origin_is(pod, 1)) continue;
        bool towed = false;
        for (int t = 0; t < ship->towed_pod_count; t++)
            towed = towed || ship->towed_pods[t] == i;
        if (towed_only && !towed) continue;
        float d = v2_dist_sq(ship->pos, pod->pos);
        if (d < distance) { distance = d; best = i; }
    }
    return best;
}

static int owned_relay_scaffold(const world_t *w) {
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        if (w->scaffolds[i].active && w->scaffolds[i].owner == 0 &&
            w->scaffolds[i].module_type == MODULE_SIGNAL_RELAY) return i;
    return -1;
}

static vec2 frontier_point(const world_t *w) {
    vec2 best = v2(0, 0);
    float distance = 1e30f;
    for (int y = -12000; y <= 18000; y += 250) {
        for (int x = -12000; x <= 12000; x += 250) {
            vec2 pos = v2((float)x, (float)y);
            if (!can_place_outpost(w, pos)) continue;
            bool tow_room = true;
            for (int side = 0; side < 8; side++) {
                vec2 edge = v2_add(pos, v2_scale(v2_from_angle((float)side * PI_F / 4), 250));
                if (!can_place_outpost(w, edge)) { tow_room = false; break; }
            }
            if (!tow_room) continue;
            float d = v2_dist_sq(pos, w->stations[1].pos);
            if (d < distance) { best = pos; distance = d; }
        }
    }
    return best;
}

static bool present_pod(world_t *w, int pod, const uint8_t secret[64]) {
    uint8_t message[SIGNED_ACTION_HEADER_SIZE + 35 + SIGNED_ACTION_SIG_SIZE] = {NET_MSG_SIGNED_ACTION};
    uint64_t nonce = w->players[0].last_signed_nonce + 1;
    for (int i = 0; i < 8; i++) message[1 + i] = (uint8_t)(nonce >> (i * 8));
    message[9] = SIGNED_ACTION_PRESENT_POD;
    write_u16_le(message + 10, 35);
    message[12] = (uint8_t)pod;
    if (!server_cargo_pod_selection_token(w, pod, message + 13)) return false;
    write_u16_le(message + 45, (uint16_t)nonce);
    signal_crypto_sign(message + 47, message + 1, 46, secret);
    uint8_t type;
    uint64_t verified_nonce;
    const uint8_t *payload;
    uint16_t size;
    if (signed_action_verify(w, 0, message, sizeof(message), &type,
            &verified_nonce, &payload, &size) != SIGNED_ACTION_OK) return false;
    w->players[0].last_signed_nonce = verified_nonce;
    server_signed_action_dispatch_result_t result;
    if (!server_dispatch_signed_action_payload(w, 0, type, payload, size,
                                               NULL, NULL, &result)) return false;
    fprintf(stderr, "present result=%d moved=%d\n", result.pod_present_result,
            result.pod_present_moved);
    return result.pod_present_evaluated && result.pod_present_result == CARGO_POD_PRESENT_OK;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: signal_first_outpost seconds fresh-chain-directory\n");
        return 2;
    }
    errno = 0;
    char *end = NULL;
    long duration = strtol(argv[1], &end, 10);
    if (errno || !end || *end || duration < 1 || duration > 3600 ||
        strlen(argv[2]) > PERSISTENCE_GENERATION_PATH_MAX - 32) {
        fprintf(stderr, "Choose 1..3600 seconds and a path shorter than 480 bytes.\n");
        return 2;
    }
#ifdef _WIN32
    int made = _mkdir(argv[2]);
#else
    int made = mkdir(argv[2], 0700);
#endif
    if (made != 0) {
        fprintf(stderr, "Choose a fresh writable chain directory: %s\n", argv[2]);
        return 2;
    }
    chain_log_set_dir(argv[2]);
    world_t *w = calloc(1, sizeof(*w));
    if (!w) return 2;
    w->rng = 2037;
    world_reset(w);
    world_seed_station_manifests(w);
    world_seed_station_chain_genesis(w);
    uint8_t pubkey[32], secret[64];
    if (!authenticate(w, pubkey, secret)) {
        world_cleanup(w);
        free(w);
        return 2;
    }
    server_player_t *sp = &w->players[0];
    player_seed_credits(sp, w);
    uint32_t starter_asset = sp->ship_asset_id;
    const float dt = 1.0f / 120.0f;
    nav_path_t path = {0};
    route_phase_t stage = PHASE_LAUNCH;
    int target = -1;
    int home = sp->current_station;
    float last_payout = 0;
    int last_pod_count = 0;
    int shipyard_wait_until = 0;
    int frame_pickup_after = 0;
    for (int i = 0; i < w->station_count; i++) {
        const station_t *st = &w->stations[i];
        fprintf(stderr, "%s %.0f %.0f frames=%d lasers=%d tractors=%d relay=%d\n", st->name, st->pos.x, st->pos.y, station_finished_count(st, COMMODITY_FRAME), station_finished_count(st, COMMODITY_LASER_MODULE), station_finished_count(st, COMMODITY_TRACTOR_MODULE), station_can_order_scaffold(st, MODULE_SIGNAL_RELAY));
    }
    bool fracture = false, tow = false, payout = false, active_outpost = false;
    bool relay_ordered = false;
    vec2 frontier = frontier_point(w);
    int checkpoint_mining = 0, checkpoint_hold = 0, checkpoint_stations = w->station_count;
    bool checkpoint_payout = false, checkpoint_order = false, checkpoint_frames = false;
    bool checkpoints_ok = true;
    printf("seconds,phase,x,y,hull,target,ore_tows,pod_tows,earned,balance,docked,speed,mining,hold,held_frames,station_frames\n");
    for (int tick = 0; tick < (int)duration * 120; tick++) {
        sp->input = (input_intent_t){.mining_target_hint = -1,
            .place_target_station = -1, .place_target_ring = -1,
            .place_target_slot = -1};
        if (home == 0 && stage < PHASE_DOCK && payout &&
            !ship_has_towed_fragments(sp->ship) && sp->ship->towed_pod_count > 0) {
            stage = PHASE_DOCK;
            path = (nav_path_t){0};
            fprintf(stderr, "physical cargo %.2f pods=%d\n", tick * dt,
                    sp->ship->towed_pod_count);
        }
        if (stage == PHASE_LAUNCH) {
            if (sp->docked) sp->input.launch = true;
            vec2 exit = station_exit_target(&w->stations[home], sp->ship->pos);
            vec2 direction = v2_norm(v2_sub(exit, w->stations[home].pos));
            exit = v2_add(w->stations[home].pos, v2_scale(direction, 1700));
            apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path, exit, 60, 200, dt));
            if (v2_dist_sq(sp->ship->pos, w->stations[home].pos) > 1450 * 1450) {
                stage = PHASE_SELECT_ROCK; path = (nav_path_t){0};
            }
        } else if (stage == PHASE_SELECT_ROCK) {
            target = nearest_rock(w, sp->ship, false, home);
            if (target >= 0) { stage = PHASE_MINE; path = (nav_path_t){0}; }
        } else if (stage == PHASE_MINE) {
            asteroid_t *a = &w->asteroids[target];
            if (!a->active || asteroid_is_collectible(a)) { stage = PHASE_COLLECT_ORE; path = (nav_path_t){0}; }
            else {
                float dist = sqrtf(v2_dist_sq(sp->ship->pos, a->pos));
                flight_cmd_t cmd = dist > a->radius + 200
                    ? flight_steer_to(w, sp->ship, &path, a->pos, a->radius + 120, 200, dt)
                    : flight_hover_near(w, sp->ship, a->pos, a->radius + 110);
                flight_avoid_station_wall(w, sp->ship, &cmd);
                apply_flight(&sp->input, cmd);
                sp->input.mine = true;
                sp->input.mining_target_hint = (int16_t)target;
            }
            if (sp->ship->stat_asteroids_fractured > 0 && !fracture) {
                fracture = true; fprintf(stderr, "fracture %.2f\n", tick * dt);
            }
        } else if (stage == PHASE_COLLECT_ORE) {
            sp->input.tractor_hold = true;
            target = nearest_rock(w, sp->ship, true, home);
            if (ship_has_towed_fragments(sp->ship)) {
                if (!tow) { tow = true; fprintf(stderr, "tow %.2f\n", tick * dt); }
                stage = PHASE_DELIVER_ORE; path = (nav_path_t){0};
            } else if (target >= 0) {
                apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path,
                    w->asteroids[target].pos, 40, 120, dt));
            } else stage = PHASE_SELECT_ROCK;
        } else if (stage == PHASE_DELIVER_ORE) {
            sp->input.tractor_hold = true;
            flight_cmd_t cmd = flight_steer_to(w, sp->ship, &path,
                drop_point(&w->stations[home]), 80, 120, dt);
            flight_avoid_station_wall(w, sp->ship, &cmd);
            apply_flight(&sp->input, cmd);
            if (sp->ship->stat_credits_earned > last_payout) {
                payout = true;
                last_payout = sp->ship->stat_credits_earned;
                fprintf(stderr, "payout %.2f earned=%.1f balance=%.1f home=%d\n", tick * dt, last_payout, ledger_balance_by_pubkey(&w->stations[home], pubkey), home);
            }
            if (!ship_has_towed_fragments(sp->ship)) {
                stage = affordable_pod(w, sp, 0, COMMODITY_FERRITE_INGOT) >= 0 ? PHASE_DOCK : PHASE_SELECT_ROCK;
                path = (nav_path_t){0};
            }
        } else if (stage == PHASE_DOCK) {
            station_t *st = &w->stations[home];
            vec2 approach = dock_route_target(st, sp->ship);
            flight_cmd_t approach_cmd = v2_dist_sq(sp->ship->pos, st->pos) < 1000 * 1000
                ? steer_through_lane(sp->ship, approach)
                : flight_steer_to(w, sp->ship, &path, approach, 40, 70, dt);
            apply_flight(&sp->input, approach_cmd);
            if (sp->in_dock_range && sp->nearby_station == home) {
                sp->input.dock = true;
                sp->input.thrust = 0;
            }
            if (sp->docked) { stage = home == 0 ? PHASE_UNPACK_INGOTS : PHASE_REFIT; fprintf(stderr, "dock %.2f\n", tick * dt); }
        } else if (stage == PHASE_REFIT) {
            station_t *st = &w->stations[home];
            if (home >= SIGNAL_FIRST_OUTPOST_INDEX) {
                sp->input.service_sell = true;
                sp->input.service_sell_only = COMMODITY_FRAME;
                if (station_is_active(st) && !st->scaffold &&
                    memcmp(st->outpost_founder_pubkey, pubkey, 32) == 0 &&
                    station_has_module(st, MODULE_SIGNAL_RELAY)) {
                    fprintf(stderr, "active outpost %.2f slot=%d\n", tick * dt, home);
                    active_outpost = true;
                    break;
                }
            } else if (!sp->ship->mining_level) {
                sp->input.upgrade_mining = true;
                sp->input.service_sell = !can_afford_upgrade(st, sp->ship,
                    SHIP_UPGRADE_MINING, ledger_balance_by_pubkey(st, pubkey));
                sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
            } else if (!relay_ordered) {
                sp->input.buy_scaffold_kit = true;
                sp->input.scaffold_kit_module = MODULE_SIGNAL_RELAY;
                if (st->pending_scaffold_count > 0 || owned_relay_scaffold(w) >= 0) {
                    relay_ordered = true;
                    fprintf(stderr, "ordered %.2f balance=%.1f\n", tick * dt,
                            ledger_balance_by_pubkey(st, pubkey));
                }
            } else if (has_undelivered_ingots(w, sp->ship, home)) {
                stage = PHASE_DELIVER_INGOTS; path = (nav_path_t){0};
            } else {
                int frames = local_frame_pod(w, sp->ship, true);
                int relay = owned_relay_scaffold(w);
                bool building = relay >= 0 && w->scaffolds[relay].state == SCAFFOLD_NASCENT;
                if (frames >= 0 && building && sp->ship->hold_level) {
                    stage = PHASE_EXIT_FRAMES; path = (nav_path_t){0};
                } else if (frames >= 0) {
                    if (!present_pod(w, frames, secret)) break;
                } else if (!sp->ship->hold_level && can_afford_upgrade(st, sp->ship,
                        SHIP_UPGRADE_HOLD, ledger_balance_by_pubkey(st, pubkey))) {
                    sp->input.upgrade_hold = true;
                } else if (sp->ship->hold_level &&
                        ship_finished_count(sp->ship, COMMODITY_FRAME) >= 48) {
                    stage = PHASE_TOW_RELAY; path = (nav_path_t){0};
                    fprintf(stderr, "activation frames %.2f held=%d\n", tick * dt,
                            ship_finished_count(sp->ship, COMMODITY_FRAME));
                } else {
                    stage = PHASE_COLLECT_FRAMES; path = (nav_path_t){0};
                }
            }
        } else if (stage == PHASE_BUY_INGOTS) {
            int pod = affordable_pod(w, sp, 0, COMMODITY_FERRITE_INGOT);
            if (sp->ship->towed_pod_count) {
                fprintf(stderr, "bought %.2f units=%d\n", tick * dt,
                    w->cargo_pods[sp->ship->towed_pods[0]].quantity);
                stage = PHASE_HAUL; path = (nav_path_t){0};
            } else if (pod >= 0) {
                sp->input.buy_product = true;
                sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
                sp->input.buy_grade = MINING_GRADE_COUNT;
                sp->input.buy_station_pod = true;
                sp->input.buy_station_pod_index = (uint16_t)pod;
            } else { stage = PHASE_LAUNCH; path = (nav_path_t){0}; }
        } else if (stage == PHASE_HAUL) {
            if (sp->docked) sp->input.launch = true;
            vec2 exit = station_exit_target(&w->stations[home], sp->ship->pos);
            vec2 direction = v2_norm(v2_sub(exit, w->stations[home].pos));
            exit = v2_add(w->stations[home].pos, v2_scale(direction, 1700));
            apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path, exit, 60,
                sp->ship->towed_pod_count ? 70 : 200, dt));
            if (v2_dist_sq(sp->ship->pos, w->stations[home].pos) > 1450 * 1450) {
                home = 1; stage = PHASE_DOCK; path = (nav_path_t){0};
            }
        } else if (stage == PHASE_UNPACK_INGOTS) {
            if (sp->ship->mining_level) {
                stage = PHASE_HAUL; path = (nav_path_t){0};
            } else if (ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT) >= 8 &&
                sp->ship->towed_pod_count < 2) {
                stage = PHASE_HAUL; path = (nav_path_t){0};
            } else if (sp->ship->towed_pod_count) {
                if (!present_pod(w, sp->ship->towed_pods[0], secret)) break;
            } else if (ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT) > 0) {
                stage = PHASE_HAUL; path = (nav_path_t){0};
            } else stage = PHASE_BUY_INGOTS;
        } else if (stage == PHASE_DELIVER_INGOTS) {
            if (sp->docked) sp->input.launch = true;
            station_t *st = &w->stations[home];
            int hopper = station_find_hopper_for(st, COMMODITY_FERRITE_INGOT);
            if (hopper < 0) break;
            vec2 goal = module_world_pos_ring(st, st->modules[hopper].ring,
                                              st->modules[hopper].slot);
            for (int m = 0; m < st->module_count; m++) {
                if (st->modules[m].scaffold || st->modules[m].type != MODULE_FRAME_PRESS)
                    continue;
                vec2 press = module_world_pos_ring(st, st->modules[m].ring,
                                                   st->modules[m].slot);
                goal = v2_scale(v2_add(goal, press), 0.5f);
                break;
            }
            apply_flight(&sp->input, steer_through_lane(sp->ship, goal));
            sp->input.tractor_hold = true;
            if (!has_undelivered_ingots(w, sp->ship, home)) {
                sp->input.release_tow = true;
                sp->input.tractor_hold = false;
                fprintf(stderr, "ingot hopper %.2f frames=%d balance=%.1f\n", tick * dt,
                    station_finished_count(st, COMMODITY_FRAME),
                    ledger_balance_by_pubkey(st, pubkey));
                stage = PHASE_DOCK; path = (nav_path_t){0};
            }
        }
        if (stage == PHASE_EXIT_FRAMES) {
            if (sp->docked) sp->input.launch = true;
            vec2 exit = exit_route_target(&w->stations[home], sp->ship);
            apply_flight(&sp->input, steer_through_lane(sp->ship, exit));
            sp->input.tractor_hold = true;
            sp->input.thrust = fminf(sp->input.thrust * 3.333333f, 1.0f);
            if (v2_dist_sq(sp->ship->pos, w->stations[home].pos) > 1450 * 1450) {
                stage = PHASE_DELIVER_FRAMES; path = (nav_path_t){0};
            }
        } else if (stage == PHASE_DELIVER_FRAMES) {
            if (sp->docked) sp->input.launch = true;
            station_t *st = &w->stations[home];
            int hopper = station_find_hopper_for(st, COMMODITY_FRAME);
            int pod = local_frame_pod(w, sp->ship, true);
            if (hopper < 0) break;
            vec2 goal = module_world_pos_ring(st, st->modules[hopper].ring,
                                              st->modules[hopper].slot);
            vec2 approach = v2_add(goal, v2_scale(v2_norm(v2_sub(goal, st->pos)), 60));
            if (v2_dist_sq(sp->ship->pos, st->pos) > 450 * 450)
                approach = dock_route_target(st, sp->ship);
            apply_flight(&sp->input, steer_through_lane(sp->ship, approach));
            sp->input.tractor_hold = true;
            sp->input.thrust = fminf(sp->input.thrust, 0.3f);
            if (pod < 0 || v2_dist_sq(w->cargo_pods[pod].pos, goal) <
                    HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE) {
                sp->input.release_tow = true;
                sp->input.tractor_hold = false;
                stage = PHASE_WAIT_FRAMES; path = (nav_path_t){0};
                shipyard_wait_until = tick + 20 * 120;
                fprintf(stderr, "shipyard frames %.2f\n", tick * dt);
            }
        } else if (stage == PHASE_WAIT_FRAMES) {
            apply_flight(&sp->input, steer_through_lane(sp->ship, sp->ship->pos));
            if (tick >= shipyard_wait_until) {
                stage = PHASE_DOCK; path = (nav_path_t){0};
            }
        } else if (stage == PHASE_COLLECT_FRAMES) {
            if (sp->docked) sp->input.launch = true;
            if (local_frame_pod(w, sp->ship, true) >= 0) {
                stage = PHASE_DOCK; path = (nav_path_t){0};
            } else if (sp->ship->towed_pod_count) {
                sp->input.release_tow = true;
                frame_pickup_after = tick + 60;
                int pod = local_frame_pod(w, sp->ship, false);
                if (pod >= 0)
                    apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path,
                        w->cargo_pods[pod].pos, 30, 100, dt));
            } else {
                int pod = local_frame_pod(w, sp->ship, false);
                if (pod >= 0) {
                    vec2 goal = w->cargo_pods[pod].pos;
                    apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path,
                                                            goal, 30, 100, dt));
                    sp->input.tractor_hold = tick >= frame_pickup_after &&
                        v2_dist_sq(sp->ship->pos, goal) < 100 * 100;
                } else {
                    fprintf(stderr, "more ore %.2f frames=%d\n", tick * dt,
                            ship_finished_count(sp->ship, COMMODITY_FRAME));
                    home = 0; stage = PHASE_LAUNCH; path = (nav_path_t){0};
                }
            }
        } else if (stage == PHASE_TOW_RELAY) {
            if (sp->docked) sp->input.launch = true;
            int scaffold = sp->ship->towed_scaffold;
            if (scaffold >= 0) {
                if (v2_dist_sq(sp->ship->pos, w->stations[1].pos) < 1000 * 1000)
                    apply_flight(&sp->input, steer_through_lane(sp->ship,
                        exit_route_target(&w->stations[1], sp->ship)));
                else
                    apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path,
                                                            frontier, 40, 70, dt));
                sp->input.tractor_hold = true;
                sp->input.thrust = fminf(sp->input.thrust, 0.3f);
                if (can_place_outpost(w, w->scaffolds[scaffold].pos))
                    sp->input.place_outpost = true;
            } else if (w->station_count > SIGNAL_FIRST_OUTPOST_INDEX) {
                home = SIGNAL_FIRST_OUTPOST_INDEX;
                stage = PHASE_DOCK; path = (nav_path_t){0};
            } else {
                scaffold = owned_relay_scaffold(w);
                if (scaffold >= 0 && w->scaffolds[scaffold].state == SCAFFOLD_LOOSE) {
                    apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path,
                        w->scaffolds[scaffold].pos, 30, 100, dt));
                    sp->input.tractor_hold = true;
                }
            }
        }
        if (!sp->input.release_tow && sp->ship->towed_pod_count > 0 && (stage == PHASE_HAUL || stage == PHASE_DOCK || stage == PHASE_DELIVER_INGOTS)) {
            sp->input.tractor_hold = true;
            sp->input.thrust = fminf(sp->input.thrust, 0.3f);
        }
        world_sim_step(w, dt);
        if (sp->ship_asset_id != starter_asset ||
            (fracture && sp->ship->stat_asteroids_fractured == 0)) {
            fprintf(stderr, "route stopped after ship loss at %.2f seconds\n", tick * dt);
            break;
        }
        bool next_payout = sp->ship->stat_credits_earned > 0;
        bool next_frames = ship_finished_count(sp->ship, COMMODITY_FRAME) >= 48;
        if ((!checkpoint_payout && next_payout) ||
            checkpoint_mining != sp->ship->mining_level ||
            checkpoint_hold != sp->ship->hold_level ||
            checkpoint_stations != w->station_count ||
            (!checkpoint_order && relay_ordered) ||
            (!checkpoint_frames && next_frames)) {
            if (checkpoint_mining != sp->ship->mining_level ||
                checkpoint_hold != sp->ship->hold_level)
                fprintf(stderr, "upgrade %.2f mining=%d hold=%d\n", tick * dt,
                        sp->ship->mining_level, sp->ship->hold_level);
            checkpoints_ok = checkpoint_roundtrip(w, argv[2], pubkey, secret);
            if (!checkpoints_ok) break;
            checkpoint_payout = next_payout;
            checkpoint_mining = sp->ship->mining_level;
            checkpoint_hold = sp->ship->hold_level;
            checkpoint_stations = w->station_count;
            checkpoint_order = relay_ordered;
            checkpoint_frames = next_frames;
        }
        if (sp->ship->towed_pod_count != last_pod_count) {
            last_pod_count = sp->ship->towed_pod_count;
            fprintf(stderr, "pod count %.2f count=%d stage=%d pos=%.0f/%.0f\n", tick * dt, last_pod_count, stage, sp->ship->pos.x, sp->ship->pos.y);
        }
        if (tick % 1200 == 0) {
            printf("%.2f,%d,%.1f,%.1f,%.1f,%d,%d,%d,%.1f,%.1f,%d,%.1f,%d,%d,%d,%d\n",
                tick * dt, stage, sp->ship->pos.x, sp->ship->pos.y,
                sp->ship->hull, target, sp->ship->towed_count,
                sp->ship->towed_pod_count, sp->ship->stat_credits_earned,
                ledger_balance_by_pubkey(&w->stations[home], pubkey),
                sp->docked, v2_len(sp->ship->vel), sp->ship->mining_level,
                sp->ship->hold_level, ship_finished_count(sp->ship, COMMODITY_FRAME),
                station_finished_count(&w->stations[home], COMMODITY_FRAME));
            fflush(stdout);
            if (stage == PHASE_WAIT_FRAMES) {
                int hopper = station_find_hopper_for(&w->stations[home], COMMODITY_FRAME);
                vec2 goal = module_world_pos_ring(&w->stations[home], w->stations[home].modules[hopper].ring,
                                                 w->stations[home].modules[hopper].slot);
                for (int p = 0; p < MAX_CARGO_PODS; p++) {
                    const cargo_pod_t *pod = &w->cargo_pods[p];
                    if (!pod->active || pod->commodity != COMMODITY_FRAME || !pod_origin_is(pod, home)) continue;
                    int ps = -1, pm = -1;
                    cargo_pod_module_tractor_indices(pod, &ps, &pm);
                    fprintf(stderr, "frame pod=%d qty=%d player=%d module=%d/%d arrived=%d hopper=%.0f charge=%lld pos=%.0f/%.0f\n",
                            p, pod->quantity, cargo_pod_player_tractor(pod), ps, pm,
                            cargo_pod_module_tractor_arrived(w, pod, ps, pm),
                            v2_len(v2_sub(pod->pos, goal)), (long long)pod->custody_charge_total, pod->pos.x, pod->pos.y);
                }
            }
            int relay = owned_relay_scaffold(w);
            if (relay >= 0 && stage >= PHASE_REFIT)
                fprintf(stderr, "relay state=%d material=%.0f/%.0f pos=%.0f/%.0f\n",
                        w->scaffolds[relay].state, w->scaffolds[relay].build_amount,
                        module_build_cost_lookup(MODULE_SIGNAL_RELAY), w->scaffolds[relay].pos.x,
                        w->scaffolds[relay].pos.y);
            if (stage >= PHASE_REFIT)
                fprintf(stderr, "refit %.1f levels=%d/%d frames=%d lasers=%d hold=%d cost=%.1f\n", tick * dt,
                    sp->ship->mining_level, sp->ship->hold_level,
                    station_finished_count(&w->stations[home], COMMODITY_FRAME),
                    station_finished_count(&w->stations[home], COMMODITY_LASER_MODULE),
                    ship_finished_count(sp->ship, COMMODITY_FRAME),
                    upgrade_station_credit_cost(&w->stations[home], sp->ship, SHIP_UPGRADE_MINING, 8));
        }
    }
    if (active_outpost)
        checkpoints_ok = checkpoint_roundtrip(w, argv[2], pubkey, secret);
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active) continue;
        fprintf(stderr, "remaining pod=%d commodity=%s quantity=%d origin=%u pos=%.0f/%.0f custody=%u\n",
                i, commodity_short_name(pod->commodity), pod->quantity,
                pod->manifest_count ? pod->manifest_units[0].origin_station : 255,
                pod->pos.x, pod->pos.y, pod->custody_station);
    }
    world_cleanup(w);
    free(w);
    return fracture && tow && payout && active_outpost && checkpoints_ok ? 0 : 1;
}
