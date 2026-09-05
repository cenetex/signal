/* Fresh-world release route. The driver supplies player inputs each tick. */
#include "game_sim.h"
#include "sim_flight.h"
#include "sim_nav.h"
#include "pubkey_proof.h"
#include "protocol.h"
#include "net_protocol.h"
#include "signal_crypto.h"
#include "chain_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                                                &proved) && proved.verified;
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
    chain_log_set_dir(argv[2]);
    world_t *w = calloc(1, sizeof(*w));
    if (!w) return 2;
    w->rng = 2037;
    world_reset(w);
    world_seed_station_manifests(w);
    world_seed_station_chain_genesis(w);
    uint8_t pubkey[32], secret[64];
    if (!authenticate(w, pubkey, secret)) return 2;
    server_player_t *sp = &w->players[0];
    player_seed_credits(sp, w);
    const float dt = 1.0f / 120.0f;
    nav_path_t path = {0};
    int stage = 0, target = -1;
    int home = sp->current_station;
    float last_payout = 0;
    int last_pod_count = 0;
    for (int i = 0; i < w->station_count; i++) {
        const station_t *st = &w->stations[i];
        fprintf(stderr, "%s %.0f %.0f frames=%d lasers=%d tractors=%d relay=%d\n", st->name, st->pos.x, st->pos.y, station_finished_count(st, COMMODITY_FRAME), station_finished_count(st, COMMODITY_LASER_MODULE), station_finished_count(st, COMMODITY_TRACTOR_MODULE), station_can_order_scaffold(st, MODULE_SIGNAL_RELAY));
    }
    bool fracture = false, tow = false, payout = false;
    printf("seconds,stage,x,y,hull,target,towed,earned,balance\n");
    for (int tick = 0; tick < (argc > 1 ? atoi(argv[1]) : 600) * 120; tick++) {
        sp->input = (input_intent_t){.mining_target_hint = -1,
            .place_target_station = -1, .place_target_ring = -1,
            .place_target_slot = -1};
        if (home == 0 && stage < 5 && payout &&
            !ship_has_towed_fragments(sp->ship) && sp->ship->towed_pod_count > 0) {
            stage = 8;
            path = (nav_path_t){0};
            fprintf(stderr, "physical cargo %.2f pods=%d\n", tick * dt,
                    sp->ship->towed_pod_count);
        }
        if (stage == 0) {
            if (sp->docked) sp->input.launch = true;
            vec2 exit = station_exit_target(&w->stations[home], sp->ship->pos);
            vec2 direction = v2_norm(v2_sub(exit, w->stations[home].pos));
            exit = v2_add(w->stations[home].pos, v2_scale(direction, 1700));
            apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path, exit, 60, 200, dt));
            if (v2_dist_sq(sp->ship->pos, w->stations[home].pos) > 1450 * 1450) {
                stage = 1; path = (nav_path_t){0};
            }
        } else if (stage == 1) {
            target = nearest_rock(w, sp->ship, false, home);
            if (target >= 0) { stage = 2; path = (nav_path_t){0}; }
        } else if (stage == 2) {
            asteroid_t *a = &w->asteroids[target];
            if (!a->active || asteroid_is_collectible(a)) { stage = 3; path = (nav_path_t){0}; }
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
        } else if (stage == 3) {
            sp->input.tractor_hold = true;
            target = nearest_rock(w, sp->ship, true, home);
            if (ship_has_towed_fragments(sp->ship)) {
                if (!tow) { tow = true; fprintf(stderr, "tow %.2f\n", tick * dt); }
                stage = 4; path = (nav_path_t){0};
            } else if (target >= 0) {
                apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path,
                    w->asteroids[target].pos, 40, 120, dt));
            } else stage = 1;
        } else if (stage == 4) {
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
                stage = affordable_pod(w, sp, 0, COMMODITY_FERRITE_INGOT) >= 0 ? 5 : 1;
                path = (nav_path_t){0};
            }
        } else if (stage == 5) {
            station_t *st = &w->stations[home];
            vec2 approach = station_approach_target(st, sp->ship->pos);
            if (!nav_segment_clear(w, sp->ship->pos, approach,
                                   ship_hull_def(sp->ship)->ship_radius + 30)) {
                vec2 entry = station_entry_target(st, sp->ship->pos);
                if (v2_dist_sq(sp->ship->pos, entry) > 140 * 140)
                    approach = entry;
            }
            apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path, approach, 40, 70, dt));
            if (sp->in_dock_range && sp->nearby_station == home) {
                sp->input.dock = true;
                sp->input.thrust = 0;
            }
            if (sp->docked) { stage = home == 0 ? 7 : 6; fprintf(stderr, "dock %.2f\n", tick * dt); }
        } else if (stage == 6) {
            if (sp->ship->towed_pod_count) {
                if (!present_pod(w, sp->ship->towed_pods[0], secret)) break;
            }
            sp->input.service_sell = ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT) > 0;
            sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
            sp->input.upgrade_mining = sp->ship->mining_level == 0;
            sp->input.upgrade_hold = !sp->input.upgrade_mining && sp->ship->hold_level == 0;
            if (sp->ship->mining_level && sp->ship->hold_level) {
                fprintf(stderr, "upgraded %.2f balance=%.1f\n", tick * dt, ledger_balance_by_pubkey(&w->stations[home], pubkey));
                break;
            }
        } else if (stage == 7) {
            int pod = affordable_pod(w, sp, 0, COMMODITY_FERRITE_INGOT);
            if (sp->ship->towed_pod_count) {
                fprintf(stderr, "bought %.2f units=%d\n", tick * dt,
                    w->cargo_pods[sp->ship->towed_pods[0]].quantity);
                stage = 8; path = (nav_path_t){0};
            } else if (pod >= 0) {
                sp->input.buy_product = true;
                sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
                sp->input.buy_grade = MINING_GRADE_COUNT;
                sp->input.buy_station_pod = true;
                sp->input.buy_station_pod_index = (uint16_t)pod;
            } else { stage = 0; path = (nav_path_t){0}; }
        } else if (stage == 8) {
            if (sp->docked) sp->input.launch = true;
            vec2 exit = station_exit_target(&w->stations[home], sp->ship->pos);
            vec2 direction = v2_norm(v2_sub(exit, w->stations[home].pos));
            exit = v2_add(w->stations[home].pos, v2_scale(direction, 1700));
            apply_flight(&sp->input, flight_steer_to(w, sp->ship, &path, exit, 60, 70, dt));
            if (v2_dist_sq(sp->ship->pos, w->stations[home].pos) > 1450 * 1450) {
                home = 1; stage = 5; path = (nav_path_t){0};
            }
        }
        if (sp->ship->towed_pod_count > 0 && (stage == 8 || stage == 5)) {
            sp->input.tractor_hold = true;
            sp->input.thrust = fminf(sp->input.thrust, 0.3f);
        }
        world_sim_step(w, dt);
        if (sp->ship->towed_pod_count != last_pod_count) {
            last_pod_count = sp->ship->towed_pod_count;
            fprintf(stderr, "pod count %.2f count=%d stage=%d pos=%.0f/%.0f\n", tick * dt, last_pod_count, stage, sp->ship->pos.x, sp->ship->pos.y);
        }
        if (tick % 1200 == 0) {
            printf("%.2f,%d,%.1f,%.1f,%.1f,%d,%d,%.1f,%.1f,docked=%d,speed=%.1f,thrust=%.2f,goal=%.1f/%.1f,tow=%.1f/%.1f\n", tick * dt,
                stage, sp->ship->pos.x, sp->ship->pos.y, sp->ship->hull,
                target, sp->ship->towed_count, sp->ship->stat_credits_earned,
                ledger_balance_by_pubkey(&w->stations[home], pubkey), sp->docked, v2_len(sp->ship->vel), sp->input.thrust, drop_point(&w->stations[home]).x, drop_point(&w->stations[home]).y, sp->ship->towed_count ? w->asteroids[sp->ship->towed_fragments[0]].pos.x : 0, sp->ship->towed_count ? w->asteroids[sp->ship->towed_fragments[0]].pos.y : 0);
            fflush(stdout);
        }
    }
    world_cleanup(w);
    free(w);
    return fracture && tow && payout ? 0 : 1;
}
