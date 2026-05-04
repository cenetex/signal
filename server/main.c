/*
 * main.c -- Headless authoritative game server for Signal Space Miner.
 *
 * Uses cesanta/mongoose for WebSocket handling.  Runs the full game
 * simulation and broadcasts state to browser clients.
 */
#include "mongoose.h"
#include "game_sim.h"
#include "highscore.h"
#include "manifest.h"
#include "mining.h"  /* mining_render_callsign for chain log copy */
#include "net_protocol.h"
#include "signal_crypto.h"
#include "sim_asteroid.h"
#include "chain_log.h"  /* signed event emission (#479 C) */
#include "cargo_receipt_issue.h"  /* portable cargo receipts (#479 D) */
#include "commodity.h"  /* station_*_price_unit (#prefix-pricing) */
#include "sha256.h"
#include <math.h>       /* lroundf */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static world_t world;
static bool running = true;
static const char *allowed_origin = NULL;
static const char *internal_token = NULL;

/* Shared HTTP response headers for API endpoints */
static char api_headers[256];

/* Layer A.3 of #479 — operational counters surfaced via /health.
 *   unsigned_action_count: state-changing actions accepted on the legacy
 *     unsigned NET_MSG_INPUT channel from a connection that *has* a
 *     registered pubkey. A non-zero value means at least one client is
 *     still on the pre-A.3 unsigned codepath; once it stays at zero
 *     across a deployment we can flip the unsigned action path off.
 *   signed_action_count: signed actions verified + dispatched.
 *   signed_action_reject_count: signed actions dropped (any reason).
 */
static uint64_t signed_action_count = 0;
static uint64_t signed_action_reject_count = 0;
static uint64_t unsigned_action_count = 0;

/* Dirty flags: only re-broadcast station identity when something changed */
static bool station_identity_dirty[MAX_STATIONS];
static bool station_econ_dirty = true;   /* station inventories changed */
static bool contracts_dirty = true;       /* contract list changed */
static highscore_table_t highscores;
static bool highscores_dirty = true;      /* broadcast + persist pending */

/* Defined further down; forward-declared so the highscore helpers can
 * use the same send wrapper as every other broadcast in this file
 * (consistent with future send-queue / rate-limiting changes). */
static void ws_send(struct mg_connection *c, const void *data, size_t len);

static void broadcast_highscores(void) {
    uint8_t buf[HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE];
    int len = highscore_serialize(buf, &highscores);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!world.players[p].connected || !world.players[p].conn) continue;
        ws_send(world.players[p].conn, buf, (size_t)len);
    }
}

static void send_highscores_to(struct mg_connection *c) {
    if (!c) return;
    uint8_t buf[HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE];
    int len = highscore_serialize(buf, &highscores);
    ws_send(c, buf, (size_t)len);
}

#define STATION_IDENTITY_FALLBACK_MS 2000
static uint64_t last_station_identity = 0;

/* Timing intervals in milliseconds */
#define SIM_TICK_MS   33    /* ~30 Hz poll rate; sim uses SIM_DT accumulator */
#define STATE_TICK_MS 50    /* 20 Hz player state broadcast */
#define WORLD_TICK_MS 100   /* 10 Hz world state broadcast */
#define SHIP_TICK_MS  250   /* 4 Hz full ship state (cargo, hull, etc.) */
#define MAX_SIM_STEPS 8     /* cap sub-steps per poll to prevent spiral */
#define SAVE_PATH "world.sav"
#define PLAYER_SAVE_DIR "saves"
#define STATION_CATALOG_DIR "stations"
#define HIGHSCORE_PATH "highscores.dat"
#define AUTOSAVE_MS 30000   /* autosave every 30 seconds */

/* ------------------------------------------------------------------ */
/* Signal handler                                                     */
/* ------------------------------------------------------------------ */

static void signal_handler(int sig) {
    (void)sig;
    running = false;
}

/* ------------------------------------------------------------------ */
/* Player management                                                  */
/* ------------------------------------------------------------------ */

static int alloc_player(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* WebSocket send helpers                                             */
/* ------------------------------------------------------------------ */

static void ws_send(struct mg_connection *c, const void *data, size_t len) {
    mg_ws_send(c, data, len, WEBSOCKET_OP_BINARY);
}

static void broadcast(const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].connected && world.players[i].session_ready && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static void broadcast_except(int exclude, const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude) continue;
        if (world.players[i].connected && world.players[i].session_ready && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static float fracture_signal_radius(vec2 pos) {
    float radius = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &world.stations[s];
        if (!station_provides_signal(st)) continue;
        if (v2_dist_sq(pos, st->pos) <= st->signal_range * st->signal_range &&
            st->signal_range > radius)
            radius = st->signal_range;
    }
    return radius;
}

static bool fracture_player_in_range(int player_id, int asteroid_idx) {
    float radius;
    if (player_id < 0 || player_id >= MAX_PLAYERS ||
        asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return false;
    if (!world.players[player_id].connected ||
        !world.players[player_id].session_ready ||
        !world.players[player_id].conn ||
        !world.asteroids[asteroid_idx].active)
        return false;
    radius = fracture_signal_radius(world.asteroids[asteroid_idx].pos);
    if (radius <= 0.0f) return false;
    return v2_dist_sq(world.players[player_id].ship.pos, world.asteroids[asteroid_idx].pos) <= radius * radius;
}

static void broadcast_fracture_updates(void) {
    uint32_t now_ms = (uint32_t)(world.time * 1000.0f);

    /* Challenges: re-broadcast to each in-range player while the window
     * is open (challenge_dirty is re-armed in step_fracture_claims).
     * Clients dedupe by fracture_id so duplicate challenges are cheap. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        fracture_claim_state_t *state = &world.fracture_claims[i];
        if (state->challenge_dirty && state->fracture_id && world.asteroids[i].active) {
            uint8_t buf[FRACTURE_CHALLENGE_SIZE];
            buf[0] = NET_MSG_FRACTURE_CHALLENGE;
            write_u32_le(&buf[1], state->fracture_id);
            memcpy(&buf[5], world.asteroids[i].fracture_seed, 32);
            write_u32_le(&buf[37], state->deadline_ms);
            write_u16_le(&buf[41], state->burst_cap);
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!fracture_player_in_range(p, i)) continue;
                ws_send(world.players[p].conn, buf, sizeof(buf));
            }
            state->challenge_dirty = false;
        }
        /* The legacy per-state resolved_dirty path is preserved for the
         * common "asteroid still alive at resolve time" case — cheap
         * and range-filtered. The pending_resolves queue below covers
         * the gnarly case (resolve + smelt in same tick). */
        if (state->resolved_dirty && state->fracture_id && world.asteroids[i].active) {
            uint8_t buf[FRACTURE_RESOLVED_SIZE];
            buf[0] = NET_MSG_FRACTURE_RESOLVED;
            write_u32_le(&buf[1], state->fracture_id);
            memcpy(&buf[5], world.asteroids[i].fragment_pub, 32);
            memcpy(&buf[37], state->best_player_pub, 32);
            buf[69] = state->best_grade;
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!fracture_player_in_range(p, i)) continue;
                ws_send(world.players[p].conn, buf, sizeof(buf));
            }
            state->resolved_dirty = false;
        }
    }

    /* Pending resolves: fracture_commit_resolution pushes here so
     * deliveries survive asteroid clear. Broadcast to every connected
     * player rather than range-filtering — the asteroid may be gone
     * so we can't compute range, and clients that never got the
     * matching challenge drop the resolve in mining_client_resolve_fracture. */
    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        pending_resolve_t *pr = &world.pending_resolves[p];
        if (!pr->active) continue;
        if (pr->tx_count > 0 && now_ms < pr->last_tx_ms + FRACTURE_RESOLVE_RETRY_PERIOD_MS)
            continue;
        uint8_t buf[FRACTURE_RESOLVED_SIZE];
        buf[0] = NET_MSG_FRACTURE_RESOLVED;
        write_u32_le(&buf[1], pr->fracture_id);
        memcpy(&buf[5], pr->fragment_pub, 32);
        memcpy(&buf[37], pr->winner_pub, 32);
        buf[69] = pr->grade;
        for (int pi = 0; pi < MAX_PLAYERS; pi++) {
            if (!world.players[pi].connected || !world.players[pi].conn) continue;
            ws_send(world.players[pi].conn, buf, sizeof(buf));
        }
        pr->tx_count++;
        pr->last_tx_ms = now_ms;
        if (pr->tx_count >= FRACTURE_RESOLVE_RETRY_COUNT) pr->active = false;
    }
}

/* ------------------------------------------------------------------ */
/* WS message handler                                                 */
/* ------------------------------------------------------------------ */

/* Per-player WebSocket message rate limiting */
static struct { uint64_t window_start; int msg_count; } ws_rate[MAX_PLAYERS];
#define WS_RATE_WINDOW_MS 1000
#define WS_RATE_LIMIT 60  /* 60 msgs/sec -- generous for 30Hz game input */

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    int pid = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].conn == c) { pid = i; break; }
    }
    if (pid < 0) return;

    /* Rate limit: silently drop excess messages */
    uint64_t now = mg_millis();
    if (now - ws_rate[pid].window_start > WS_RATE_WINDOW_MS) {
        ws_rate[pid].window_start = now;
        ws_rate[pid].msg_count = 0;
    }
    if (++ws_rate[pid].msg_count > WS_RATE_LIMIT) return;

    const uint8_t *data = (const uint8_t *)wm->data.buf;
    int len = (int)wm->data.len;
    if (len < 1 || pid < 0 || pid >= MAX_PLAYERS) return;

    switch (data[0]) {
    case NET_MSG_INPUT:
        parse_input(data, len, &world.players[pid].input);
        /* If the player just queued a shipyard order, refresh that station's
         * identity on the next world tick so the SHIPYARD tab sees the new
         * pending count immediately instead of waiting for the 2s fallback. */
        if (len >= 3) {
            uint8_t action = data[2];
            if ((action >= NET_ACTION_BUY_SCAFFOLD_TYPED &&
                 action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) ||
                action == NET_ACTION_BUY_SCAFFOLD) {
                int s = world.players[pid].current_station;
                if (s >= 0 && s < MAX_STATIONS) station_identity_dirty[s] = true;
            }
            /* Layer A.3 of #479 — track state-changing actions that
             * arrived on the unsigned channel from a client that has
             * a registered pubkey. Once this counter stays at zero
             * across a deployment, the unsigned action path can be
             * removed entirely. NET_ACTION_NONE (=0) is a transient-
             * input-only frame and isn't counted. */
            if (action != NET_ACTION_NONE && world.players[pid].pubkey_set) {
                unsigned_action_count++;
            }
        }
        break;
    case NET_MSG_PLAN:
        parse_plan(data, len, &world.players[pid].input);
        break;
    case NET_MSG_STATE:
        /* Ignored -- server is authoritative. */
        break;
    case NET_MSG_MINING_ACTION:
        /* Legacy -- mining handled via INPUT flags now. */
        break;
    case NET_MSG_BUY_INGOT:
        /* RATi v2: purchase a specific named ingot from the docked
         * station's manifest. Payload: [type:1][pubkey:32]. The unit
         * is transferred from station.manifest to ship.manifest with
         * its full provenance (prefix_class, origin_station,
         * mined_block, parent_merkle) preserved. */
        if (len >= 33 && world.players[pid].docked) {
            int sidx = world.players[pid].current_station;
            if (sidx < 0 || sidx >= MAX_STATIONS) break;
            server_player_t *sp = &world.players[pid];
            station_t *st = &world.stations[sidx];
            ship_t *ship = &sp->ship;
            const uint8_t *pk = &data[1];
            int slot = manifest_find(&st->manifest, pk);
            if (slot < 0) break;
            cargo_unit_t *src = &st->manifest.units[slot];
            if ((cargo_kind_t)src->kind != CARGO_KIND_INGOT) break;
            /* Prefix-class price multipliers (#prefix-pricing): the
             * specific unit's sale price scales by both the dynamic
             * stock curve and the unit's prefix_class. Anonymous
             * ingots aren't purchasable through this path — they're
             * bulk material and have no named-collectible premium. */
            if ((ingot_prefix_t)src->prefix_class == INGOT_PREFIX_ANONYMOUS) break;
            int price = (int)lroundf(station_sell_price_unit(st, src));
            if (price <= 0) break;
            /* Use ledger_spend so the credit pool stays conserved. */
            bool spent = sp->pubkey_set
                ? ledger_spend_by_pubkey(st, sp->pubkey, (float)price, ship)
                : ledger_spend(st, sp->session_token, (float)price, ship);
            if (!spent) break;
            cargo_unit_t copy = *src;
            if (!ship->manifest.units && !ship_manifest_bootstrap(ship)) break;
            if (!manifest_push(&ship->manifest, &copy)) break;
            (void)manifest_remove(&st->manifest, (uint16_t)slot, NULL);
            st->manifest_dirty = true;
            /* Layer C of #479: emit EVT_TRANSFER + EVT_TRADE. The two
             * are linked by transfer_event_id so a verifier can stitch
             * them back into a single atomic move.
             * Layer D of #479: also issue a portable cargo_receipt_t the
             * player carries with the cargo. The origin receipt's
             * prev_receipt_hash is the station's chain_last_hash AFTER
             * the EVT_TRANSFER emit — verifiable in isolation by
             * walking the station's chain log to that exact event. */
            {
                cargo_receipt_t receipt;
                uint64_t xfer_id = cargo_receipt_emit_transfer(
                    &world, st,
                    st->station_pubkey,
                    world.players[pid].pubkey,
                    copy.pub,
                    (uint8_t)CARGO_KIND_INGOT,
                    st->chain_last_hash, /* anchor: post-emit hash */
                    &receipt);
                if (xfer_id != 0) {
                    /* Attach receipt to the just-pushed manifest entry.
                     * manifest_push appended at index manifest.count - 1. */
                    ship_receipts_t *rcpts = ship_get_receipts(ship);
                    if (rcpts) {
                        (void)ship_receipts_push_chain(rcpts, &receipt, 1);
                        /* Keep parity: receipts.count must mirror manifest.count. */
                    }
                    /* Fire NET_MSG_CARGO_RECEIPT_BUNDLE to the client. */
                    uint8_t buf[3 + CARGO_RECEIPT_SIZE];
                    buf[0] = NET_MSG_CARGO_RECEIPT_BUNDLE;
                    buf[1] = 1;
                    buf[2] = 0;
                    cargo_receipt_pack(&receipt, &buf[3]);
                    ws_send(c, buf, sizeof(buf));

                    chain_payload_trade_t trade = {0};
                    trade.transfer_event_id = xfer_id;
                    trade.ledger_delta_signed = -(int64_t)price;
                    memcpy(trade.ledger_pubkey, world.players[pid].pubkey, 32);
                    (void)chain_log_emit(&world, st, CHAIN_EVT_TRADE,
                                         &trade, (uint16_t)sizeof(trade));
                }
            }
            char cs[12]; mining_render_callsign(copy.pub, cs);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s purchased %s for %d", world.players[pid].callsign, cs, price);
            signal_channel_post(&world, sidx, msg, "");
        }
        break;
    case NET_MSG_DELIVER_INGOT:
        /* RATi v2: deposit a specific hold ingot into the docked
         * station's manifest. Payload: [type:1][hold_index:1]. The
         * index is into ship.manifest filtered by named ingots
         * (kind == INGOT && prefix != ANONYMOUS). */
        if (len >= 2 && world.players[pid].docked) {
            int sidx = world.players[pid].current_station;
            if (sidx < 0 || sidx >= MAX_STATIONS) break;
            station_t *st = &world.stations[sidx];
            ship_t *ship = &world.players[pid].ship;
            int target = data[1];
            int hidx = -1;
            int seen = 0;
            for (uint16_t u = 0; u < ship->manifest.count; u++) {
                const cargo_unit_t *cu = &ship->manifest.units[u];
                if ((cargo_kind_t)cu->kind != CARGO_KIND_INGOT) continue;
                if ((ingot_prefix_t)cu->prefix_class == INGOT_PREFIX_ANONYMOUS) continue;
                if (seen == target) { hidx = (int)u; break; }
                seen++;
            }
            if (hidx < 0) break;
            cargo_unit_t copy = ship->manifest.units[hidx];
            /* Layer D of #479: validate any attached receipt chain
             * before accepting. If the chain fails verification, refuse
             * the deliver — federation invariant: a station only takes
             * cargo whose lineage is signed all the way back. If no
             * receipt chain is attached (legacy / pre-D save / cargo
             * smelted on-station with no transfer history), accept
             * unconditionally — the station treats the loading-state
             * cargo as origin-attested and signs a fresh receipt. */
            ship_receipts_t *rcpts = ship_get_receipts(ship);
            if (rcpts && hidx < (int)rcpts->count) {
                const cargo_receipt_chain_t *attached = &rcpts->chains[hidx];
                if (attached->len > 0) {
                    cargo_receipt_result_t vr = cargo_receipt_chain_verify(
                        attached->links, attached->len, copy.pub);
                    if (vr != CARGO_RECEIPT_OK) {
                        printf("[server] receipt_chain_invalid: deliver from player %d, reason=%d\n",
                               pid, (int)vr);
                        break; /* refuse the deliver */
                    }
                    if (attached->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
                        printf("[server] receipt_chain_cap_exceeded: deliver from player %d\n", pid);
                        break;
                    }
                }
            }
            /* FIFO-evict the oldest manifest entry on full station, mirroring
             * the smelt rotation path. The evicted unit's pubkey is voided
             * so it can never be re-deposited. */
            if (st->manifest.count >= st->manifest.cap) {
                cargo_unit_t evicted = {0};
                if (manifest_remove(&st->manifest, 0, &evicted) &&
                    (ingot_prefix_t)evicted.prefix_class != INGOT_PREFIX_ANONYMOUS) {
                    char ev_cs[12]; mining_render_callsign(evicted.pub, ev_cs);
                    char ev_msg[96];
                    snprintf(ev_msg, sizeof(ev_msg), "stockpile full — voided %s", ev_cs);
                    signal_channel_post(&world, sidx, ev_msg, "");
                }
            }
            if (!manifest_push(&st->manifest, &copy)) break;
            /* Capture last receipt hash (for the new station-issued
             * receipt's prev_receipt_hash) BEFORE removing. */
            uint8_t prev_hash[32] = {0};
            bool have_prev = false;
            if (rcpts && hidx < (int)rcpts->count) {
                const cargo_receipt_chain_t *attached = &rcpts->chains[hidx];
                if (attached->len > 0) {
                    cargo_receipt_hash(&attached->links[attached->len - 1], prev_hash);
                    have_prev = true;
                }
            }
            (void)manifest_remove(&ship->manifest, (uint16_t)hidx, NULL);
            if (rcpts && hidx < (int)rcpts->count) {
                (void)ship_receipts_remove(rcpts, (uint16_t)hidx, NULL);
            }
            /* Pay delivery credit through the ledger so supply stays
             * balanced. Prefix-class price multipliers (#501): a specific
             * delivered unit pays station_buy_price_unit, so M-class
             * ingots pay 2× and RATi pays 50×. INGOT_DELIVERY_CREDIT is
             * kept as the floor for low-base-price edge cases. */
            float delivery_f = station_buy_price_unit(st, &copy);
            float floor_f = (float)INGOT_DELIVERY_CREDIT;
            if (delivery_f < floor_f) delivery_f = floor_f;
            int delivery_int = (int)lroundf(delivery_f);
            if (world.players[pid].pubkey_set) {
                ledger_credit_supply_by_pubkey(st, world.players[pid].pubkey, (float)delivery_int);
            } else {
                ledger_credit_supply(st, world.players[pid].session_token, (float)delivery_int);
            }
            st->manifest_dirty = true;
            /* Layer C of #479: emit EVT_TRANSFER (player -> station) +
             * EVT_TRADE (delivery credit accrual on the station's
             * ledger).
             * Layer D of #479: also issue station's own receipt. The
             * receipt's prev_receipt_hash is the SHA-256 of the player's
             * presented chain head if there was one — this hop closes
             * the chain at the destination station. If no prior chain,
             * anchor to the station's own chain_last_hash post-emit. */
            {
                cargo_receipt_t receipt;
                uint64_t xfer_id = cargo_receipt_emit_transfer(
                    &world, st,
                    world.players[pid].pubkey,
                    st->station_pubkey,
                    copy.pub,
                    (uint8_t)CARGO_KIND_INGOT,
                    have_prev ? prev_hash : st->chain_last_hash,
                    &receipt);
                if (xfer_id != 0) {
                    /* Send the destination's reissued receipt back to
                     * the player. The cargo is now in the station; the
                     * receipt is what the player would carry if the
                     * cargo ever flowed back to them. For now it's a
                     * confirmation token. */
                    uint8_t buf[3 + CARGO_RECEIPT_SIZE];
                    buf[0] = NET_MSG_CARGO_RECEIPT_BUNDLE;
                    buf[1] = 1;
                    buf[2] = 0;
                    cargo_receipt_pack(&receipt, &buf[3]);
                    ws_send(c, buf, sizeof(buf));

                    /* Trade event records the actual prefix-scaled
                     * delivery amount (#501), not a flat constant. */
                    chain_payload_trade_t trade = {0};
                    trade.transfer_event_id = xfer_id;
                    trade.ledger_delta_signed = (int64_t)delivery_int;
                    memcpy(trade.ledger_pubkey, world.players[pid].pubkey, 32);
                    (void)chain_log_emit(&world, st, CHAIN_EVT_TRADE,
                                         &trade, (uint16_t)sizeof(trade));
                }
            }
            char cs[12]; mining_render_callsign(copy.pub, cs);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s delivered %s", world.players[pid].callsign, cs);
            signal_channel_post(&world, sidx, msg, "");
        }
        break;
    case NET_MSG_FRACTURE_CLAIM:
        if (len >= FRACTURE_CLAIM_SIZE) {
            uint32_t fracture_id = read_u32_le(&data[1]);
            uint32_t burst_nonce = read_u32_le(&data[5]);
            uint8_t claimed_grade = data[9];
            (void)submit_fracture_claim(&world, pid, fracture_id, burst_nonce, claimed_grade);
        }
        break;
    case NET_MSG_SIGNED_ACTION: {
        /* Layer A.3 of #479 — Ed25519-signed state-changing action. */
        uint8_t action_type = 0;
        uint64_t nonce = 0;
        const uint8_t *payload = NULL;
        uint16_t payload_len = 0;
        signed_action_result_t res = signed_action_verify(
            &world, pid, data, len,
            &action_type, &nonce, &payload, &payload_len);
        if (res != SIGNED_ACTION_OK) {
            signed_action_reject_count++;
            const char *reason = "unknown";
            switch (res) {
            case SIGNED_ACTION_REJECT_NO_PUBKEY:    reason = "no-pubkey"; break;
            case SIGNED_ACTION_REJECT_MALFORMED:    reason = "malformed"; break;
            case SIGNED_ACTION_REJECT_BAD_SIG:      reason = "bad-sig";   break;
            case SIGNED_ACTION_REJECT_REPLAY:       reason = "replay";    break;
            case SIGNED_ACTION_REJECT_UNKNOWN_TYPE: reason = "unk-type";  break;
            default: break;
            }
            const uint8_t *pk = world.players[pid].pubkey;
            printf("[server] signed-action rejected (%s) from player %d pk=%02x%02x%02x%02x...\n",
                   reason, pid, pk[0], pk[1], pk[2], pk[3]);
            break;
        }
        /* Verified — commit the nonce high-water mark BEFORE dispatch
         * so a faulting handler can't clear the replay protection. */
        world.players[pid].last_signed_nonce = nonce;
        signed_action_count++;
        server_player_t *sp = &world.players[pid];
        switch ((signed_action_type_t)action_type) {
        case SIGNED_ACTION_BUY_PRODUCT:
            /* Payload: [commodity:1][grade:1] — same fields the unsigned
             * NET_MSG_INPUT.action path produces. We just stuff them into
             * the same intent slot the sim already consumes. */
            if (payload_len >= 2) {
                uint8_t commodity = payload[0];
                uint8_t grade     = payload[1];
                if (commodity < COMMODITY_COUNT) {
                    sp->input.buy_product = true;
                    sp->input.buy_commodity = (commodity_t)commodity;
                    if (grade <= MINING_GRADE_COUNT)
                        sp->input.buy_grade = (mining_grade_t)grade;
                    else
                        sp->input.buy_grade = MINING_GRADE_COUNT;
                }
            }
            break;
        case SIGNED_ACTION_BUY_INGOT:
            /* Payload: [pubkey:32]. Reuses the existing NET_MSG_BUY_INGOT
             * handler logic by re-entering it with a synthesized buffer.
             * Cheap and avoids duplicating the manifest transfer code. */
            if (payload_len >= 32 && sp->docked) {
                int sidx = sp->current_station;
                if (sidx >= 0 && sidx < MAX_STATIONS) {
                    station_t *st = &world.stations[sidx];
                    ship_t *ship = &sp->ship;
                    int slot = manifest_find(&st->manifest, payload);
                    if (slot >= 0) {
                        cargo_unit_t *src = &st->manifest.units[slot];
                        if ((cargo_kind_t)src->kind == CARGO_KIND_INGOT &&
                            (ingot_prefix_t)src->prefix_class != INGOT_PREFIX_ANONYMOUS) {
                            /* Prefix-class price multipliers (#prefix-pricing):
                             * mirror the unsigned BUY_INGOT path above. */
                            int price = (int)lroundf(station_sell_price_unit(st, src));
                            bool spent = price > 0 && (sp->pubkey_set
                                ? ledger_spend_by_pubkey(st, sp->pubkey, (float)price, ship)
                                : ledger_spend(st, sp->session_token, (float)price, ship));
                            if (spent) {
                                cargo_unit_t copy = *src;
                                if ((ship->manifest.units || ship_manifest_bootstrap(ship)) &&
                                    manifest_push(&ship->manifest, &copy)) {
                                    (void)manifest_remove(&st->manifest, (uint16_t)slot, NULL);
                                    st->manifest_dirty = true;
                                }
                            }
                        }
                    }
                }
            }
            break;
        case SIGNED_ACTION_SELL_CARGO:
            /* Payload: [commodity:1][grade:1]. commodity==COMMODITY_COUNT
             * means "sell all" (legacy bulk path). */
            if (payload_len >= 2) {
                uint8_t commodity = payload[0];
                uint8_t grade     = payload[1];
                sp->input.service_sell = true;
                sp->input.service_sell_only =
                    (commodity < COMMODITY_COUNT)
                    ? (commodity_t)commodity : COMMODITY_COUNT;
                if (grade < MINING_GRADE_COUNT) {
                    sp->input.service_sell_grade = (mining_grade_t)grade;
                    sp->input.service_sell_one = true;
                } else {
                    sp->input.service_sell_grade = MINING_GRADE_COUNT;
                    sp->input.service_sell_one = false;
                }
            }
            break;
        case SIGNED_ACTION_PLACE_OUTPOST:
            if (payload_len >= 3) {
                sp->input.place_outpost = true;
                sp->input.place_target_station = (int8_t)payload[0];
                sp->input.place_target_ring    = (int8_t)payload[1];
                sp->input.place_target_slot    = (int8_t)payload[2];
            }
            break;
        case SIGNED_ACTION_FRACTURE_CLAIM:
            if (payload_len >= 9) {
                uint32_t fracture_id = (uint32_t)payload[0]
                                     | ((uint32_t)payload[1] << 8)
                                     | ((uint32_t)payload[2] << 16)
                                     | ((uint32_t)payload[3] << 24);
                uint32_t burst_nonce = (uint32_t)payload[4]
                                     | ((uint32_t)payload[5] << 8)
                                     | ((uint32_t)payload[6] << 16)
                                     | ((uint32_t)payload[7] << 24);
                uint8_t claimed_grade = payload[8];
                (void)submit_fracture_claim(&world, pid, fracture_id,
                                            burst_nonce, claimed_grade);
            }
            break;
        case SIGNED_ACTION_DELIVER:
        case SIGNED_ACTION_CLAIM_CONTRACT:
        case SIGNED_ACTION_CANCEL_CONTRACT:
            /* Wire path defined; dispatcher reuses existing intent slots
             * once the corresponding client paths are migrated. Until
             * the client sends these, the signed channel is happy to
             * verify them but the sim still consumes them via the
             * unsigned NET_MSG_INPUT path. */
            break;
        case SIGNED_ACTION_COUNT:
        default:
            /* unreachable — verify rejected unknown types */
            break;
        }
        break;
    }
    case NET_MSG_REGISTER_PUBKEY:
        /* Layer A.2 of #479: client asserts its persisted Ed25519 pubkey.
         * TODO(#479-A.3): unauthenticated registration — A.3 will require
         * the client to sign every input and the server to verify. Until
         * then, an attacker on the network can spoof another player's
         * pubkey, but cannot act as them because identity at the wire
         * level is still the 8-byte session_token. */
        if (len >= REGISTER_PUBKEY_MSG_SIZE) {
            const uint8_t *pk = &data[1];
            server_player_t *sp = &world.players[pid];
            /* Idempotent: same pubkey + already-set => no-op. */
            if (sp->pubkey_set && memcmp(sp->pubkey, pk, 32) == 0) break;
            /* If this pubkey already maps to a different (live) player slot
             * — i.e. a returning player whose token rotated — transfer the
             * persistent state from the old slot into this connection's
             * slot so the player resumes their save by pubkey, then
             * disconnect the old slot. Only meaningful once SESSION has
             * already populated the per-slot session_token of the existing
             * record; a registry entry pointing at no live slot just gets
             * rebound below. */
            int existing = registry_lookup_by_pubkey(&world, pk);
            if (existing >= 0 && existing != pid) {
                server_player_t *old = &world.players[existing];
                if (sp->session_ready &&
                    memcmp(old->session_token, sp->session_token, 8) != 0) {
                    /* Transfer persistent state. The old slot's ledger
                     * balances live in station_t.ledger[] keyed by the
                     * old session_token — they survive the swap because
                     * we copy the old token's ship + station context into
                     * the new slot but leave ledger entries untouched.
                     * For A.2 we model "carry the player record across"
                     * by adopting the old slot's session_token: future
                     * ledger reads on this connection use the old token,
                     * so the manifest + ledger are preserved.
                     *
                     * NOTE: this is a best-effort A.2 reconcile —
                     * cross-token ledger merging is out of scope and
                     * lands with A.3 / A.4. */
                    if (ship_copy(&sp->ship, &old->ship)) {
                        sp->current_station = old->current_station;
                        sp->nearby_station = old->nearby_station;
                        sp->docked = old->docked;
                        sp->in_dock_range = old->in_dock_range;
                        /* Migrate ledger entries from the old token →
                         * the new connection's session_token across
                         * every station, so balances stay spendable
                         * after the rebinding.
                         *
                         * Compare and write the FULL 32-byte pseudokey
                         * form (token bytes + 24 trailing zeros — see
                         * token_to_pseudo_pubkey). The old version did
                         * memcmp/memcpy on 8 bytes against a 32-byte
                         * field, which (a) could partially-match a real
                         * Ed25519 pubkey by 1/2^64 chance and (b) left
                         * the trailing 24 bytes of a touched entry
                         * holding stale data, orphaning the entry from
                         * future lookups. Both bugs are silent —
                         * 32-byte compare + 32-byte write fixes them. */
                        uint8_t old_pseudo[32] = {0};
                        uint8_t new_pseudo[32] = {0};
                        memcpy(old_pseudo, old->session_token, 8);
                        memcpy(new_pseudo, sp->session_token, 8);
                        for (int s = 0; s < MAX_STATIONS; s++) {
                            station_t *st = &world.stations[s];
                            for (int e = 0; e < st->ledger_count; e++) {
                                if (memcmp(st->ledger[e].player_pubkey, old_pseudo, 32) == 0) {
                                    memcpy(st->ledger[e].player_pubkey,
                                           new_pseudo, 32);
                                }
                            }
                        }
                        old->connected = false;
                        old->grace_period = false;
                        old->conn = NULL;
                        memset(old->session_token, 0, 8);
                        old->session_ready = false;
                        uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)existing };
                        broadcast(leave_old, 2);
                        printf("[server] player %d: pubkey reconnect (was slot %d)\n",
                               pid, existing);
                    }
                }
            }
            memcpy(sp->pubkey, pk, 32);
            sp->pubkey_set = true;
            /* Bind / rebind the registry to this slot's session_token.
             * If session_token isn't ready yet (REGISTER_PUBKEY arrived
             * before SESSION, the expected order), bind to the
             * placeholder zero token; the SESSION handler rebinds once
             * the real token is known. */
            (void)registry_register_pubkey(&world, pk, sp->session_token);
            printf("[server] player %d: registered pubkey %02x%02x%02x%02x...\n",
                   pid, pk[0], pk[1], pk[2], pk[3]);

            /* Layer A.4 of #479: try to restore the player's record from
             * a pubkey-keyed save. If none exists, advertise legacy saves
             * (if any) so the client can present a claim UI. */
            if (player_load_by_pubkey(sp, &world, PLAYER_SAVE_DIR, pk)) {
                printf("[server] player %d: restored save by pubkey\n", pid);
            } else {
                char prefixes[LEGACY_SAVES_MAX_LIST][LEGACY_SAVES_PREFIX_LEN + 1];
                char names[LEGACY_SAVES_MAX_LIST][64];
                int n = player_save_list_legacy(PLAYER_SAVE_DIR, prefixes, names,
                                                LEGACY_SAVES_MAX_LIST);
                if (n > 0) {
                    uint8_t buf[LEGACY_SAVES_HEADER +
                                LEGACY_SAVES_MAX_LIST * LEGACY_SAVES_PREFIX_LEN];
                    buf[0] = NET_MSG_LEGACY_SAVES_AVAILABLE;
                    buf[1] = (uint8_t)n;
                    for (int i = 0; i < n; i++) {
                        memcpy(&buf[LEGACY_SAVES_HEADER + i * LEGACY_SAVES_PREFIX_LEN],
                               prefixes[i], LEGACY_SAVES_PREFIX_LEN);
                    }
                    ws_send(c, buf,
                            (size_t)(LEGACY_SAVES_HEADER + n * LEGACY_SAVES_PREFIX_LEN));
                    printf("[server] player %d: advertised %d legacy save(s)\n",
                           pid, n);
                }
            }
        }
        break;
    case NET_MSG_CLAIM_LEGACY_SAVE: {
        /* Layer A.4 of #479. Client supplies (token_hex, signature). We
         * verify sig against the registered pubkey, then rename the
         * legacy save to the pubkey-keyed path and load it. */
        if (len < 2) break;
        server_player_t *sp = &world.players[pid];
        if (!sp->pubkey_set) break;
        uint8_t hex_len = data[1];
        if (hex_len == 0 || hex_len > 64) break;
        if (len < (int)(2 + hex_len + SIGNED_ACTION_SIG_SIZE)) break;
        const uint8_t *hex = &data[2];
        const uint8_t *sig = &data[2 + hex_len];

        /* Reject any non-hex byte to keep the basename safe for filesystem. */
        for (int i = 0; i < hex_len; i++) {
            uint8_t ch = hex[i];
            bool digit = (ch >= '0' && ch <= '9');
            bool lower = (ch >= 'a' && ch <= 'f');
            bool upper = (ch >= 'A' && ch <= 'F');
            if (!digit && !lower && !upper) {
                printf("[server] player %d: claim rejected (bad hex)\n", pid);
                goto claim_done;
            }
        }

        /* Reconstruct the signed message: domain || token_hex. */
        const char *domain = CLAIM_LEGACY_SAVE_DOMAIN;
        size_t dlen = strlen(domain);
        uint8_t msg[64 + 64];
        if (dlen + hex_len > sizeof(msg)) goto claim_done;
        memcpy(msg, domain, dlen);
        memcpy(msg + dlen, hex, hex_len);
        if (!signal_crypto_verify(sig, msg, dlen + hex_len, sp->pubkey)) {
            printf("[server] player %d: claim signature invalid\n", pid);
            goto claim_done;
        }

        /* The wire format carries the full token base name *without*
         * the "player_" prefix or the .sav suffix; legacy saves on disk
         * use either the "player_<hex>" form (token-keyed) or
         * "player_<slot>" form (anonymous slot fallback). Accept either:
         * try the literal name first, then with the historical prefix. */
        char basename[80];
        if ((size_t)hex_len + 1 > sizeof(basename)) goto claim_done;
        memcpy(basename, hex, hex_len);
        basename[hex_len] = '\0';

        bool ok = player_save_rename_legacy_to_pubkey(PLAYER_SAVE_DIR,
                                                       basename, sp->pubkey);
        if (!ok) {
            char prefixed[96];
            snprintf(prefixed, sizeof(prefixed), "player_%s", basename);
            ok = player_save_rename_legacy_to_pubkey(PLAYER_SAVE_DIR,
                                                     prefixed, sp->pubkey);
        }
        if (!ok) {
            printf("[server] player %d: claim rename failed (race / missing)\n", pid);
            goto claim_done;
        }
        if (player_load_by_pubkey(sp, &world, PLAYER_SAVE_DIR, sp->pubkey)) {
            printf("[server] player %d: claimed legacy save\n", pid);
        }
    claim_done:
        break;
    }
    case NET_MSG_SESSION:
        if (len >= 9 && !world.players[pid].session_ready) {
            const uint8_t *token = &data[1];
            /* Extract callsign if present (bytes 9-15) */
            if (len >= 16) {
                memcpy(world.players[pid].callsign, &data[9], 7);
                world.players[pid].callsign[7] = '\0';
                printf("[server] player %d callsign: %s\n", pid, world.players[pid].callsign);
            }
            /* Check for existing grace-period player with same token */
            int reattach = -1;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (i == pid) continue;
                if (world.players[i].connected && world.players[i].grace_period &&
                    world.players[i].session_ready &&
                    memcmp(world.players[i].session_token, token, 8) == 0) {
                    reattach = i;
                    break;
                }
            }
            if (reattach >= 0) {
                /* Reattach: copy state from grace slot to new slot */
                server_player_t *old = &world.players[reattach];
                server_player_t *sp = &world.players[pid];
                if (!ship_copy(&sp->ship, &old->ship)) break;
                sp->current_station = old->current_station;
                sp->nearby_station = old->nearby_station;
                sp->docked = old->docked;
                sp->in_dock_range = old->in_dock_range;
                memcpy(sp->session_token, token, 8);
                sp->session_ready = true;
                /* Clear the grace slot and broadcast LEAVE so clients drop the ghost */
                old->connected = false;
                old->grace_period = false;
                old->conn = NULL;
                uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)reattach };
                broadcast(leave_old, 2);
                printf("[server] player %d: reconnected (was slot %d)\n", pid, reattach);
            } else {
                memcpy(world.players[pid].session_token, token, 8);
                world.players[pid].session_ready = true;
                /* Try to restore saved state keyed by session token */
                if (player_load_by_token(&world.players[pid], &world,
                                         PLAYER_SAVE_DIR, token)) {
                    printf("[server] player %d: restored save by session\n", pid);
                } else {
                    printf("[server] player %d: no save for session, fresh ship\n", pid);
                }
                /* Seed starting credits now that session_token is set */
                player_seed_credits(&world.players[pid], &world);
            }
            /* Layer A.2 (#479): if the client registered its pubkey before
             * SESSION, rebind the registry entry to the real session_token
             * so future lookups by pubkey find this slot. */
            if (world.players[pid].pubkey_set) {
                (void)registry_register_pubkey(&world, world.players[pid].pubkey,
                                               world.players[pid].session_token);
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Station REST API                                                   */
/* ------------------------------------------------------------------ */

static const char *api_token = NULL;

static bool api_auth_ok(struct mg_http_message *hm) {
    if (!api_token || api_token[0] == '\0') return false;
    struct mg_str *auth = mg_http_get_header(hm, "Authorization");
    if (!auth) return false;
    /* Expect "Bearer <token>" */
    if (auth->len < 8) return false;
    const char *prefix = "Bearer ";
    if (strncmp(auth->buf, prefix, 7) != 0) return false;
    return strncmp(auth->buf + 7, api_token, auth->len - 7) == 0
        && strlen(api_token) == auth->len - 7;
}

static bool internal_auth_ok(struct mg_http_message *hm) {
    if (!internal_token || internal_token[0] == '\0') return false;
    struct mg_str *auth = mg_http_get_header(hm, "X-Internal-Token");
    if (!auth) return false;
    return strncmp(auth->buf, internal_token, auth->len) == 0
        && strlen(internal_token) == auth->len;
}

/* Validate that a byte sequence is valid UTF-8. */
static bool is_valid_utf8(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c < 0x80) {
            /* ASCII */
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (i + 1 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if (i + 2 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if ((data[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            if (i + 3 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if ((data[i + 2] & 0xC0) != 0x80) return false;
            if ((data[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

static int parse_station_id(struct mg_http_message *hm) {
    /* Extract station index from /api/station/<id>/... */
    /* URI looks like /api/station/0/state or /api/station/2/command */
    const char *p = hm->uri.buf + 13; /* skip "/api/station/" */
    const char *end = hm->uri.buf + hm->uri.len;
    if (p >= end) return -1;
    /* Length-safe integer parse: only read digits up to next '/' or URI end */
    int id = 0;
    bool has_digit = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_digit = true;
        id = id * 10 + (*p - '0');
        if (id >= MAX_STATIONS) return -1;
        p++;
    }
    if (!has_digit) return -1;
    if (id < 0 || id >= MAX_STATIONS) return -1;
    if (!station_exists(&world.stations[id])) return -1;
    return id;
}

/* Append a JSON-escaped string to buf at *pos, respecting bufsz.
 * Escapes quotes, backslashes, and control characters. */
static void json_escape_append(char *buf, int *pos, int bufsz, const char *s) {
    int p = *pos;
    if (p >= bufsz - 1) return;
    for (; *s && p < bufsz - 1; s++) {
        char esc = 0;
        switch (*s) {
            case '"':  esc = '"';  break;
            case '\\': esc = '\\'; break;
            case '\n': esc = 'n';  break;
            case '\r': esc = 'r';  break;
            case '\t': esc = 't';  break;
        }
        if (esc) {
            if (p + 2 > bufsz - 1) break;
            buf[p++] = '\\';
            buf[p++] = esc;
        } else if ((unsigned char)*s < 0x20) {
            /* Other control chars: emit as \u00XX */
            if (p + 6 > bufsz - 1) break;
            p += snprintf(buf + p, (size_t)(bufsz - p), "\\u%04x", (unsigned char)*s);
        } else {
            buf[p++] = *s;
        }
    }
    *pos = p;
}

/* Safe snprintf append: clamp pos to bufsz before snprintf to avoid undefined behavior */
#define BUF_APPEND(pos, buf, bufsz, ...) do { \
    if ((pos) < (bufsz)) { \
        int _n = snprintf((buf) + (pos), (size_t)((bufsz) - (pos)), __VA_ARGS__); \
        if (_n > 0) (pos) += _n; \
        if ((pos) > (bufsz)) (pos) = (bufsz); \
    } \
} while (0)

/* Cap visible_asteroids in the agent-facing JSON. Populated stations
 * at 15-18k signal range can see 1000+ rocks once chunk gen has run,
 * and at ~80 bytes/record that explodes past any reasonable response
 * buffer. Agents don't need 1000 rocks — the nearest N is plenty. */
#define STATION_API_MAX_ASTEROIDS 150
/* Safety margin left in the buffer after the asteroid loop so the
 * trailing players/stations/contracts/hail fields and their braces
 * always fit and the JSON closes cleanly. */
#define STATION_API_TAIL_MARGIN   2048

static void handle_station_state(struct mg_connection *c, int sid, struct mg_http_message *hm) {
    const station_t *st = &world.stations[sid];

    /* Parse query params */
    int include_activity = 0;
    char tmp[32];
    if (hm && mg_http_get_var(&hm->query, "include", tmp, sizeof(tmp)) > 0) {
        include_activity = strcmp(tmp, "activity_history") == 0;
    }
    /* Heap-allocated so we aren't bound by the event-loop thread's
     * stack (alpine musl main stack is ~80KB by default). */
    enum { BUFSZ = 131072 };
    char *buf = (char *)malloc(BUFSZ);
    if (!buf) {
        mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
        return;
    }
    int pos = 0;

    /* Station info */
    BUF_APPEND(pos, buf, BUFSZ,
        "{\"station\":{\"index\":%d,\"name\":\"", sid);
    json_escape_append(buf, &pos, BUFSZ, st->name);
    BUF_APPEND(pos, buf, BUFSZ,
        "\",\"signal_range\":%.1f,\"scaffold\":%s,"
        "\"inventory\":{",
        st->signal_range, st->scaffold ? "true" : "false");

    static const char *cnames[] = {
        "ferrite_ore","cuprite_ore","crystal_ore",
        "ferrite_ingot","cuprite_ingot","crystal_ingot",
        "frame","laser_module","tractor_module",
        "repair_kit",
    };
    _Static_assert(sizeof(cnames)/sizeof(cnames[0]) == COMMODITY_COUNT,
                   "cnames must stay in sync with commodity_t");
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "\"%s\":%.1f", cnames[i], st->_inventory_cache[i]);
    }
    BUF_APPEND(pos, buf, BUFSZ, "},\"modules\":[");
    for (int m = 0; m < st->module_count; m++) {
        if (m > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"type\":\"%s\",\"ring\":%d,\"slot\":%d,\"scaffold\":%s,\"progress\":%.2f}",
            module_type_name(st->modules[m].type),
            st->modules[m].ring, st->modules[m].slot,
            st->modules[m].scaffold ? "true" : "false",
            st->modules[m].build_progress);
    }

    BUF_APPEND(pos, buf, BUFSZ, "]},");

    /* Visible asteroids within signal range. Capped at
     * STATION_API_MAX_ASTEROIDS — agents don't need 1000+ rocks and
     * serializing them all blew past 32KB and truncated the tail of
     * the JSON mid-field (prod bug, April 2026). */
    float sr_sq = st->signal_range * st->signal_range;
    BUF_APPEND(pos, buf, BUFSZ, "\"visible_asteroids\":[");
    bool first = true;
    int asteroid_count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &world.asteroids[i];
        if (!a->active) continue;
        if (v2_dist_sq(a->pos, st->pos) > sr_sq) continue;
        if (asteroid_count >= STATION_API_MAX_ASTEROIDS) break;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"tier\":%d,\"commodity\":%d,\"x\":%.0f,\"y\":%.0f,\"hp\":%.0f}",
            i, a->tier, a->commodity, a->pos.x, a->pos.y, a->hp);
        asteroid_count++;
        if (pos > BUFSZ - STATION_API_TAIL_MARGIN) break;
    }

    /* Visible players within signal range */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_players\":[");
    first = true;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected || world.players[i].grace_period) continue;
        if (v2_dist_sq(world.players[i].ship.pos, st->pos) > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"id\":%d,\"x\":%.0f,\"y\":%.0f,\"docked\":%s}",
            i, world.players[i].ship.pos.x, world.players[i].ship.pos.y,
            world.players[i].docked ? "true" : "false");
    }

    /* Visible stations within signal range */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_stations\":[");
    first = true;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (i == sid || !station_exists(&world.stations[i])) continue;
        float d_sq = v2_dist_sq(world.stations[i].pos, st->pos);
        if (d_sq > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        float overlap = st->signal_range + world.stations[i].signal_range - sqrtf(d_sq);
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"name\":\"", i);
        json_escape_append(buf, &pos, BUFSZ, world.stations[i].name);
        BUF_APPEND(pos, buf, BUFSZ,
            "\",\"x\":%.0f,\"y\":%.0f,\"signal_overlap\":%s}",
            world.stations[i].pos.x, world.stations[i].pos.y,
            overlap > 0.0f ? "true" : "false");
    }

    /* Active contracts */
    BUF_APPEND(pos, buf, BUFSZ, "],\"active_contracts\":[");
    first = true;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        const contract_t *ct = &world.contracts[i];
        if (!ct->active || ct->station_index != sid) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"action\":%d,\"commodity\":%d,\"quantity\":%.0f,\"base_price\":%.1f,\"age\":%.0f}",
            i, ct->action, ct->commodity, ct->quantity_needed, ct->base_price, ct->age);
    }

    /* Top N most-recent dockers (relationships, #257) — bounded for prompt context.
     * Sort by last_dock_tick DESC to surface recent visitors first. */
    BUF_APPEND(pos, buf, BUFSZ, "],\"relationships\":[");
    enum { MAX_RELATIONSHIPS_IN_API = 8 };
    /* Find indices with non-zero last_dock_tick, sort by tick descending */
    int rel_indices[16];
    int rel_count = 0;
    for (int i = 0; i < st->ledger_count; i++) {
        if (st->ledger[i].last_dock_tick > 0) {
            rel_indices[rel_count++] = i;
        }
    }
    /* Simple sort — bubble sort for small N */
    for (int i = 0; i < rel_count - 1; i++) {
        for (int j = 0; j < rel_count - 1 - i; j++) {
            if (st->ledger[rel_indices[j]].last_dock_tick < st->ledger[rel_indices[j+1]].last_dock_tick) {
                int swap = rel_indices[j];
                rel_indices[j] = rel_indices[j+1];
                rel_indices[j+1] = swap;
            }
        }
    }
    first = true;
    int rel_output = 0;
    for (int i = 0; i < rel_count && rel_output < MAX_RELATIONSHIPS_IN_API; i++) {
        int idx = rel_indices[i];
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"pubkey\":\"");
        /* Encode pubkey as hex for JSON */
        for (int j = 0; j < 32; j++)
            BUF_APPEND(pos, buf, BUFSZ, "%02x", st->ledger[idx].player_pubkey[j]);
        BUF_APPEND(pos, buf, BUFSZ,
            "\",\"first_dock_tick\":%llu,\"last_dock_tick\":%llu,"
            "\"total_docks\":%u,\"lifetime_ore_units\":%u,"
            "\"lifetime_credits_in\":%u,\"lifetime_credits_out\":%u,"
            "\"top_commodity\":%u}",
            (unsigned long long)st->ledger[idx].first_dock_tick,
            (unsigned long long)st->ledger[idx].last_dock_tick,
            st->ledger[idx].total_docks,
            st->ledger[idx].lifetime_ore_units,
            st->ledger[idx].lifetime_credits_in,
            st->ledger[idx].lifetime_credits_out,
            st->ledger[idx].top_commodity);
        rel_output++;
        if (pos > BUFSZ - STATION_API_TAIL_MARGIN) break;
    }

    /* Close the "relationships" array opened above. The original
     * single-call form ("],\"hail\":...") got split across the optional
     * activity_history block; the close now fires unconditionally so the
     * JSON stays well-formed regardless of include_activity. */
    BUF_APPEND(pos, buf, BUFSZ, "]");

    /* Activity history (24-hour window, if requested) */
    if (include_activity) {
        double window_start = world.time - 86400.0;
        double ore_sum = 0.0;
        int recent_docks = 0;

        for (int i = 0; i < st->ledger_count; i++) {
            if (st->ledger[i].last_dock_tick > window_start) {
                ore_sum += st->ledger[i].lifetime_ore_units;
                recent_docks++;
            }
        }

        /* Top haulers: up to 3 players by lifetime ore contributed */
        int top_indices[3] = {-1, -1, -1};
        for (int i = 0; i < st->ledger_count; i++) {
            for (int j = 0; j < 3; j++) {
                if (top_indices[j] < 0 ||
                    st->ledger[i].lifetime_ore_units > st->ledger[top_indices[j]].lifetime_ore_units) {
                    /* Shift down */
                    for (int k = 2; k > j; k--) top_indices[k] = top_indices[k-1];
                    top_indices[j] = i;
                    break;
                }
            }
        }

        BUF_APPEND(pos, buf, BUFSZ, ",\"activity_history\":{\"ore_processed_24h\":%.0f,"
            "\"ships_docked_24h\":%d,\"top_haulers\":[",
            ore_sum, recent_docks);

        for (int j = 0; j < 3; j++) {
            if (top_indices[j] < 0) break;
            if (j > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ, "\"");
            for (int k = 0; k < 32; k++) {
                BUF_APPEND(pos, buf, BUFSZ, "%02x", st->ledger[top_indices[j]].player_pubkey[k]);
            }
            BUF_APPEND(pos, buf, BUFSZ, "\"");
        }

        BUF_APPEND(pos, buf, BUFSZ, "]}");
    }

    /* Hail message */
    BUF_APPEND(pos, buf, BUFSZ, ",\"hail\":\"");
    json_escape_append(buf, &pos, BUFSZ, st->hail_message);
    BUF_APPEND(pos, buf, BUFSZ, "\"}");

    mg_http_reply(c, 200, api_headers, "%s", buf);
    free(buf);
}

static void handle_station_command(struct mg_connection *c, struct mg_http_message *hm, int sid) {
    station_t *st = &world.stations[sid];
    struct mg_str body = hm->body;
    char *action = mg_json_get_str(body, "$.action");
    long commodity = mg_json_get_long(body, "$.commodity", -1);
    double price_val = 0;
    mg_json_get_num(body, "$.price", &price_val);
    long module_type = mg_json_get_long(body, "$.module_type", -1);
    char *hail = mg_json_get_str(body, "$.hail");
    char *currency = mg_json_get_str(body, "$.currency_name");

    if (!action) {
        mg_http_reply(c, 400, api_headers,
                      "{\"ok\":false,\"error\":\"missing action\"}");
        free(hail);
        free(currency);
        return;
    }

    if (strcmp(action, "set_hail") == 0 && hail && hail[0] != '\0') {
        snprintf(st->hail_message, sizeof(st->hail_message), "%s", hail);
        station_identity_dirty[sid] = true;
        mg_http_reply(c, 200, api_headers,
                      "{\"ok\":true,\"action\":\"set_hail\"}");
    } else if (strcmp(action, "set_currency_name") == 0 && currency && currency[0] != '\0') {
        /* ASCII-ish trim; drop anything that would mess with the HUD
         * renderer (control chars, quotes). 31 chars max so the wire
         * serializer's null terminator survives. */
        char sanitized[32] = {0};
        int out = 0;
        for (int i = 0; currency[i] && out < (int)sizeof(sanitized) - 1; i++) {
            unsigned char ch = (unsigned char)currency[i];
            if (ch < 0x20 || ch == 0x7F || ch == '"' || ch == '\\') continue;
            sanitized[out++] = (char)ch;
        }
        if (out == 0) {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"currency_name empty after sanitize\"}");
        } else {
            memcpy(st->currency_name, sanitized, sizeof(sanitized));
            station_identity_dirty[sid] = true;
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_currency_name\",\"value\":\"%s\"}", sanitized);
        }
    } else if (strcmp(action, "set_price") == 0 && commodity >= 0 && commodity < COMMODITY_COUNT && price_val > 0) {
        /* Clamp to 0.5x-2.0x of default */
        float default_price = st->base_price[commodity];
        float clamped = (float)price_val;
        if (clamped < default_price * 0.5f) clamped = default_price * 0.5f;
        if (clamped > default_price * 2.0f) clamped = default_price * 2.0f;
        st->base_price[commodity] = clamped;
        mg_http_reply(c, 200, api_headers,
                      "{\"ok\":true,\"action\":\"set_price\",\"commodity\":%ld,\"price\":%.1f}", commodity, clamped);
    } else if (strcmp(action, "build_module") == 0 && module_type >= 0 && module_type < MODULE_COUNT) {
        if (st->module_count >= MAX_MODULES_PER_STATION) {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"station full\"}");
        } else {
            begin_module_construction(&world, st, sid, (module_type_t)module_type);
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"build_module\",\"type\":%ld}", module_type);
        }
    } else {
        mg_http_reply(c, 400, api_headers,
                      "{\"ok\":false,\"error\":\"unknown action\"}");
    }
    free(action);
    free(hail);
    free(currency);
}

/* ------------------------------------------------------------------ */
/* Mongoose event handler                                             */
/* ------------------------------------------------------------------ */

/* REST API token-bucket rate limiter: 20 tokens/sec, 40 burst cap */
static uint64_t api_rate_last_refill = 0;
static int api_rate_bucket = 40;
#define API_RATE_REFILL_PER_SEC 20
#define API_RATE_BUCKET_MAX 40

static bool api_rate_check(void) {
    uint64_t now = mg_millis();
    uint64_t elapsed = now - api_rate_last_refill;
    if (elapsed >= 50) {  /* refill every 50ms to smooth out bursts */
        int refill = (int)(elapsed * API_RATE_REFILL_PER_SEC / 1000);
        if (refill > 0) {
            api_rate_bucket += refill;
            if (api_rate_bucket > API_RATE_BUCKET_MAX)
                api_rate_bucket = API_RATE_BUCKET_MAX;
            api_rate_last_refill = now;
        }
    }
    if (api_rate_bucket <= 0) return false;
    api_rate_bucket--;
    return true;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/station/*/state"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else if (sid >= 3 && !api_auth_ok(hm)) {
                    /* Seeded stations (0-2) are read-only without auth;
                     * player-built outposts (3+) require auth. */
                    mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
                } else {
                    handle_station_state(c, sid, hm);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/command"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else {
                    handle_station_command(c, hm, sid);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/signal_channel"), NULL)) {
            /* Station posts to the broadcast log (#316). */
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else {
                    char *text = mg_json_get_str(hm->body, "$.text");
                    char *audio = mg_json_get_str(hm->body, "$.audio_url");
                    if (!text || text[0] == '\0' || strlen(text) > SIGNAL_CHANNEL_TEXT_MAX - 1) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"ok\":false,\"error\":\"text missing or >200 chars\"}");
                    } else if (audio && audio[0] && strncmp(audio, "https://", 8) != 0) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"ok\":false,\"error\":\"audio_url must be https\"}");
                    } else if (audio && strlen(audio) > SIGNAL_CHANNEL_AUDIO_MAX - 1) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"ok\":false,\"error\":\"audio_url too long\"}");
                    } else {
                        uint64_t id = signal_channel_post(&world, sid, text, audio ? audio : "");
                        uint32_t ts = (uint32_t)(world.time * 1000.0f);
                        /* Push snapshot to every connected ship so the
                         * Network tab updates in-game without polling. */
                        {
                            size_t cap = (size_t)(3 + world.signal_channel.count * SIGNAL_CHANNEL_RECORD_SIZE);
                            uint8_t *msg = (uint8_t *)malloc(cap);
                            if (msg) {
                                int len = serialize_signal_channel(msg, &world.signal_channel);
                                broadcast(msg, (size_t)len);
                                free(msg);
                            }
                        }
                        mg_http_reply(c, 200, api_headers,
                                      "{\"ok\":true,\"id\":%llu,\"timestamp\":%u}",
                                      (unsigned long long)id, ts);
                    }
                    free(text);
                    free(audio);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/signal_channel/messages"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                /* Parse ?since=<id>&limit=<1..100> — crude query scan
                 * since mongoose gives us hm->query as a raw string. */
                long since = 0, limit = 50;
                char tmp[32];
                if (mg_http_get_var(&hm->query, "since", tmp, sizeof(tmp)) > 0) since = atol(tmp);
                if (mg_http_get_var(&hm->query, "limit", tmp, sizeof(tmp)) > 0) limit = atol(tmp);
                if (limit < 1) limit = 1;
                if (limit > 100) limit = 100;

                /* Response sized for worst case: 100 × 440 bytes + framing ≈ 50KB. */
                enum { RESP_BUFSZ = 65536 };
                char *out = (char *)malloc(RESP_BUFSZ);
                if (!out) {
                    mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
                } else {
                    int pos = 0;
                    BUF_APPEND(pos, out, RESP_BUFSZ, "{\"messages\":[");
                    bool first = true;
                    int emitted = 0;
                    for (int i = 0; i < world.signal_channel.count && emitted < (int)limit; i++) {
                        const signal_channel_msg_t *m = signal_channel_at(&world, i);
                        if (!m || (long long)m->id <= since) continue;
                        if (!first) BUF_APPEND(pos, out, RESP_BUFSZ, ",");
                        first = false;
                        BUF_APPEND(pos, out, RESP_BUFSZ,
                            "{\"id\":%llu,\"timestamp\":%u,\"sender_station_id\":%d,\"text\":\"",
                            (unsigned long long)m->id, m->timestamp_ms, (int)m->sender_station);
                        json_escape_append(out, &pos, RESP_BUFSZ, m->text);
                        BUF_APPEND(pos, out, RESP_BUFSZ, "\"");
                        if (m->audio_url[0]) {
                            BUF_APPEND(pos, out, RESP_BUFSZ, ",\"audio_url\":\"");
                            json_escape_append(out, &pos, RESP_BUFSZ, m->audio_url);
                            BUF_APPEND(pos, out, RESP_BUFSZ, "\"");
                        }
                        BUF_APPEND(pos, out, RESP_BUFSZ, "}");
                        emitted++;
                    }
                    BUF_APPEND(pos, out, RESP_BUFSZ, "]}");
                    mg_http_reply(c, 200, api_headers, "%s", out);
                    free(out);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/health"), NULL)) {
            int count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (world.players[i].connected) count++;
#ifdef GIT_HASH
            mg_http_reply(c, 200, api_headers,
                          "{\"status\":\"ok\",\"players\":%d,\"version\":\"%s\","
                          "\"signed_action_count\":%llu,"
                          "\"signed_action_reject_count\":%llu,"
                          "\"unsigned_action_count\":%llu,"
                          "\"hopper_smelt_events\":%llu,"
                          "\"hopper_smelt_units\":%.3f}",
                          count, GIT_HASH,
                          (unsigned long long)signed_action_count,
                          (unsigned long long)signed_action_reject_count,
                          (unsigned long long)unsigned_action_count,
                          (unsigned long long)world.hopper_smelt_events,
                          world.hopper_smelt_units);
#else
            mg_http_reply(c, 200, api_headers,
                          "{\"status\":\"ok\",\"players\":%d,\"version\":\"dev\","
                          "\"signed_action_count\":%llu,"
                          "\"signed_action_reject_count\":%llu,"
                          "\"unsigned_action_count\":%llu,"
                          "\"hopper_smelt_events\":%llu,"
                          "\"hopper_smelt_units\":%.3f}",
                          count,
                          (unsigned long long)signed_action_count,
                          (unsigned long long)signed_action_reject_count,
                          (unsigned long long)unsigned_action_count,
                          (unsigned long long)world.hopper_smelt_events,
                          world.hopper_smelt_units);
#endif
        } else if (mg_match(hm->uri, mg_str("/internal/v1/operator-post"), NULL)) {
            if (!internal_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                /* Parse JSON payload: { station_index, kind, tier, ref_id, text } */
                double station_idx_val = 0;
                mg_json_get_num(hm->body, "$.station_index", &station_idx_val);
                int station_idx = (int)station_idx_val;

                double kind_val = 0;
                mg_json_get_num(hm->body, "$.kind", &kind_val);
                uint8_t kind = (uint8_t)kind_val;

                double tier_val = 0;
                mg_json_get_num(hm->body, "$.tier", &tier_val);
                uint8_t tier = (uint8_t)tier_val;

                double ref_id_val = 0;
                mg_json_get_num(hm->body, "$.ref_id", &ref_id_val);
                uint16_t ref_id = (uint16_t)ref_id_val;

                char *text = mg_json_get_str(hm->body, "$.text");

                /* Validate inputs */
                if (station_idx < 0 || station_idx >= MAX_STATIONS) {
                    mg_http_reply(c, 400, api_headers,
                                  "{\"error\":\"invalid station_index\"}");
                } else if (!station_exists(&world.stations[station_idx])) {
                    mg_http_reply(c, 404, api_headers,
                                  "{\"error\":\"station not found\"}");
                } else if (!text || text[0] == '\0') {
                    mg_http_reply(c, 400, api_headers,
                                  "{\"error\":\"text missing\"}");
                } else {
                    size_t text_len = strlen(text);
                    if (text_len > 256) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"error\":\"text too long\",\"code\":\"text_too_long\"}");
                    } else if (!is_valid_utf8((const uint8_t *)text, text_len)) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"error\":\"invalid utf-8\"}");
                    } else {
                        /* Build the payload: fixed prefix + text */
                        size_t payload_len = 38 + text_len;
                        uint8_t *payload = (uint8_t *)malloc(payload_len);
                        if (!payload) {
                            mg_http_reply(c, 500, api_headers,
                                          "{\"error\":\"out of memory\"}");
                        } else {
                            /* Pack the fixed-prefix part */
                            payload[0] = kind;
                            payload[1] = tier;
                            payload[2] = (uint8_t)(ref_id & 0xFF);
                            payload[3] = (uint8_t)((ref_id >> 8) & 0xFF);
                            /* Compute SHA-256 of text */
                            sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
                            /* text_len field (uint16_t little-endian) */
                            payload[36] = (uint8_t)(text_len & 0xFF);
                            payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
                            /* Copy text bytes */
                            if (text_len > 0) {
                                memcpy(&payload[38], text, text_len);
                            }

                            /* Emit the signed event */
                            uint64_t event_id = chain_log_emit(&world,
                                                               &world.stations[station_idx],
                                                               CHAIN_EVT_OPERATOR_POST,
                                                               payload, (uint16_t)payload_len);
                            free(payload);

                            if (event_id > 0) {
                                mg_http_reply(c, 200, api_headers,
                                              "{\"ok\":true,\"event_id\":%llu,\"prev_hash\":\"%lX\"}",
                                              (unsigned long long)event_id,
                                              (unsigned long)(world.stations[station_idx].chain_last_hash[0]));
                            } else {
                                mg_http_reply(c, 500, api_headers,
                                              "{\"error\":\"failed to emit event\"}");
                            }
                        }
                    }
                    if (text) free(text);
                }
            }
        } else {
            mg_http_reply(c, 404, "", "Not found");
        }
    } else if (ev == MG_EV_WS_OPEN) {
        /* Per-IP connection limit to mitigate slot exhaustion */
        #define MAX_CONNS_PER_IP 4
        {
            int ip_count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (world.players[i].connected && world.players[i].conn) {
                    struct mg_connection *pc = (struct mg_connection *)world.players[i].conn;
                    if (memcmp(&pc->rem.addr, &c->rem.addr, sizeof(c->rem.addr)) == 0)
                        ip_count++;
                }
            }
            if (ip_count >= MAX_CONNS_PER_IP) {
                printf("[server] per-IP limit reached for connection, rejecting\n");
                mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
                return;
            }
        }
        int pid = alloc_player();
        if (pid < 0) {
            mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
            return;
        }
        server_player_t *sp = &world.players[pid];
        memset(sp, 0, sizeof(*sp));
        sp->connected = true;
        sp->id = (uint8_t)pid;
        sp->conn = c;
        sp->session_ready = false;
        sp->grace_timer = 5.0f;  /* Must send SESSION within 5 seconds */
        /* Start with fresh ship — save is loaded when client sends SESSION */
        player_init_ship(sp, &world);
        printf("[server] player %d: awaiting session token\n", pid);

        /* Send JOIN to new player (their own ID). */
        uint8_t join_msg[] = { NET_MSG_JOIN, (uint8_t)pid };
        ws_send(c, join_msg, 2);

        /* Notify others and tell new player about existing players. */
        broadcast_except(pid, join_msg, 2);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pid || !world.players[i].connected) continue;
            if (world.players[i].grace_period) continue; /* skip ghosts */
            uint8_t exist_msg[] = { NET_MSG_JOIN, (uint8_t)i };
            ws_send(c, exist_msg, 2);
        }

        /* Send station identity for all active stations. */
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&world.stations[s])) continue;
            uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
            int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
            ws_send(c, id_buf, (size_t)id_len);
        }

        /* Send full asteroid sync to new player and mark all as sent. */
        {
            uint8_t sync_buf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
            int sync_len = serialize_asteroids_full(sync_buf, world.asteroids);
            ws_send(c, sync_buf, (size_t)sync_len);
            /* Initialize per-player sent tracking */
            server_player_t *new_sp = &world.players[pid];
            for (int ai = 0; ai < MAX_ASTEROIDS; ai++)
                new_sp->asteroid_sent[ai] = world.asteroids[ai].active;
        }

        /* Global highscores: newcomer gets the current leaderboard so the
         * death cinematic can render it before they've played a run. */
        send_highscores_to(c);

        /* Signal channel snapshot (#316): newcomer gets the full ring
         * buffer so the Network tab has content immediately. */
        if (world.signal_channel.count > 0) {
            size_t cap = (size_t)(3 + world.signal_channel.count * SIGNAL_CHANNEL_RECORD_SIZE);
            uint8_t *msg = (uint8_t *)malloc(cap);
            if (msg) {
                int len = serialize_signal_channel(msg, &world.signal_channel);
                ws_send(c, msg, (size_t)len);
                free(msg);
            }
        }

        /* RATi v2: per-station named-ingot snapshot, derived from the
         * unified manifest. New client sees what's currently on offer
         * at every station so the MARKET stockpile UI is populated
         * immediately. Wire shape unchanged. */
        for (int sidx = 0; sidx < MAX_STATIONS; sidx++) {
            if (!station_exists(&world.stations[sidx])) continue;
            uint8_t buf[STATION_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
            int len = serialize_station_ingots(buf, sidx, &world.stations[sidx]);
            if (len <= STATION_INGOTS_HEADER) continue;
            ws_send(c, buf, (size_t)len);
        }

        /* Phase 2 station manifest summary — same rationale, different
         * payload: grade-grouped counts for the TRADE BUY rows. Sent for
         * every station (empty manifest is legal — body will be just the
         * 4-byte header with entry_count=0). */
        for (int sidx = 0; sidx < MAX_STATIONS; sidx++) {
            if (!station_exists(&world.stations[sidx])) continue;
            uint8_t mbuf[STATION_MANIFEST_HEADER +
                         COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY];
            int mlen = serialize_station_manifest(mbuf, sidx, &world.stations[sidx]);
            ws_send(c, mbuf, (size_t)mlen);
        }

        /* Send server version hash. */
        {
#ifdef GIT_HASH
            const char *hash = GIT_HASH;
#else
            const char *hash = "dev";
#endif
            size_t hlen = strlen(hash);
            uint8_t info_msg[12] = { NET_MSG_SERVER_INFO };
            if (hlen > 11) hlen = 11;
            memcpy(&info_msg[1], hash, hlen);
            ws_send(c, info_msg, 1 + hlen);
        }

        printf("[server] player %d joined\n", pid);
    } else if (ev == MG_EV_WS_MSG) {
        handle_ws_message(c, ev_data);
    } else if (ev == MG_EV_CLOSE) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (world.players[i].conn == c) {
                player_save(&world.players[i], PLAYER_SAVE_DIR, i);
                world.players[i].conn = NULL;
                if (world.players[i].session_ready) {
                    /* Keep slot alive for reconnect grace window */
                    world.players[i].grace_period = true;
                    world.players[i].grace_timer = 30.0f;
                    printf("[server] player %d disconnected, grace window 30s\n", i);
                } else {
                    /* No session — immediate full disconnect */
                    world.players[i].connected = false;
                    uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                    broadcast(leave_msg, 2);
                    printf("[server] player %d left (no session)\n", i);
                }
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Broadcast helpers                                                  */
/* ------------------------------------------------------------------ */

static void broadcast_player_states(void) {
    /* Batch all connected player states into one message, send once per client.
     * This is O(N) sends instead of O(N^2). */
    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int len = serialize_all_player_states(buf, world.players);
    broadcast(buf, (size_t)len);
}

/* mark_visible_asteroids_dirty removed — per-player relevance filtering
 * in serialize_asteroids_for_player handles viewport culling. */

static void broadcast_world(void) {
    /* Asteroids: per-player relevance filtering.
     * Each player gets only asteroids in their view radius.
     * Deactivation records sent when asteroids leave a player's view. */
    {
        uint8_t abuf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->conn) continue;
            int alen = serialize_asteroids_for_player(
                abuf, world.asteroids, sp->ship.pos, sp->asteroid_sent);
            if (alen > 2) /* skip empty messages */
                ws_send(sp->conn, abuf, (size_t)alen);
        }
        /* Bulk clear dirty flags after all players served */
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            world.asteroids[i].net_dirty = false;
    }

    /* NPCs: per-player view filtering (same radius as asteroids) */
    {
        uint8_t nbuf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->conn) continue;
            int count = 0;
            for (int i = 0; i < MAX_NPC_SHIPS; i++) {
                if (!world.npc_ships[i].active) continue;
                if (v2_dist_sq(world.npc_ships[i].ship.pos, sp->ship.pos) > ASTEROID_VIEW_RADIUS_SQ)
                    continue;
                const npc_ship_t *n = &world.npc_ships[i];
                uint8_t *q = &nbuf[2 + count * NPC_RECORD_SIZE];
                q[0] = (uint8_t)i;
                q[1] = 1;
                q[1] |= (((uint8_t)n->role & 0x3) << 1);
                q[1] |= (((uint8_t)n->state & 0x7) << 3);
                if (n->thrusting) q[1] |= (1 << 6);
                write_f32_le(&q[2],  n->ship.pos.x);
                write_f32_le(&q[6],  n->ship.pos.y);
                write_f32_le(&q[10], n->ship.vel.x);
                write_f32_le(&q[14], n->ship.vel.y);
                write_f32_le(&q[18], n->ship.angle);
                q[22] = (uint8_t)(int8_t)n->target_asteroid;
                q[23] = (uint8_t)(n->tint_r * 255.0f);
                q[24] = (uint8_t)(n->tint_g * 255.0f);
                q[25] = (uint8_t)(n->tint_b * 255.0f);
                count++;
            }
            nbuf[0] = NET_MSG_WORLD_NPCS;
            nbuf[1] = (uint8_t)count;
            if (count > 0)
                ws_send(sp->conn, nbuf, (size_t)(2 + count * NPC_RECORD_SIZE));
        }
    }

    /* Scaffolds: per-player view filtering */
    {
        uint8_t scbuf[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->conn) continue;
            int count = 0;
            for (int i = 0; i < MAX_SCAFFOLDS; i++) {
                if (!world.scaffolds[i].active) continue;
                if (v2_dist_sq(world.scaffolds[i].pos, sp->ship.pos) > ASTEROID_VIEW_RADIUS_SQ)
                    continue;
                serialize_one_scaffold(&scbuf[2 + count * SCAFFOLD_RECORD_SIZE], i, &world.scaffolds[i]);
                count++;
            }
            scbuf[0] = NET_MSG_WORLD_SCAFFOLDS;
            scbuf[1] = (uint8_t)count;
            if (count > 0)
                ws_send(sp->conn, scbuf, (size_t)(2 + count * SCAFFOLD_RECORD_SIZE));
        }
    }

    /* World time sync (5 bytes: type + float) */
    uint8_t tbuf[5];
    tbuf[0] = NET_MSG_WORLD_TIME;
    write_f32_le(&tbuf[1], world.time);
    broadcast(tbuf, 5);
}

/* Compute station-local balance for a player at their current/nearby
 * station. Must read the same ledger entry the buy/credit paths use:
 * pubkey when registered, session-token-pseudokey otherwise. Reading
 * the wrong entry was the visible-bug-symptom that motivated the
 * earlier identity-fix series — broadcast balance came from a stale
 * (often negative) session-token entry while real earnings sat on
 * the pubkey entry. */
static float player_station_balance(const server_player_t *sp) {
    int st = sp->docked ? sp->current_station : sp->nearby_station;
    if (st < 0 || st >= MAX_STATIONS) return 0.0f;
    if (sp->pubkey_set)
        return ledger_balance_by_pubkey(&world.stations[st], sp->pubkey);
    return ledger_balance(&world.stations[st], sp->session_token);
}

static int send_player_ship(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    return serialize_player_ship_bal(buf, id, sp, player_station_balance(sp));
}

static void broadcast_ship_states(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected || !world.players[i].conn) continue;
        uint8_t buf[PLAYER_SHIP_SIZE + 4]; /* +4 headroom */
        int len = send_player_ship(buf, (uint8_t)i, &world.players[i]);
        /* Full ship state sent only to the owning player. */
        ws_send(world.players[i].conn, buf, (size_t)len);

        /* RATi v2: also push hold-ingot snapshot, derived from the
         * ship manifest. Wire shape unchanged. Sized for the wire cap
         * (u8 count) so an unusually full hold can't truncate. */
        uint8_t hbuf[HOLD_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
        int hlen = serialize_hold_ingots(hbuf, &world.players[i].ship);
        ws_send(world.players[i].conn, hbuf, (size_t)hlen);

        /* Player manifest summary — keeps the trade UI's SELL rows in
         * sync with server-authoritative manifest mutations (buy/sell/
         * smelt move units across the player's manifest server-side, but
         * PLAYER_SHIP only carries the float cargo). Worst case is
         * COMMODITY_COUNT * MINING_GRADE_COUNT entries; we cap header
         * + entries with a generous bound. */
        uint8_t pmbuf[PLAYER_MANIFEST_HEADER
                      + COMMODITY_COUNT * MINING_GRADE_COUNT * PLAYER_MANIFEST_ENTRY];
        int pmlen = serialize_player_manifest(pmbuf, &world.players[i].ship);
        ws_send(world.players[i].conn, pmbuf, (size_t)pmlen);
    }

    if (station_econ_dirty) {
        uint8_t sbuf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
        int slen = serialize_stations(sbuf, world.stations);
        broadcast(sbuf, (size_t)slen);
        station_econ_dirty = false;
    }

    if (contracts_dirty) {
        uint8_t cbuf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
        int clen = serialize_contracts(cbuf, world.contracts);
        broadcast(cbuf, (size_t)clen);
        contracts_dirty = false;
    }
}

/* ================================================================== */
/* sim_event handlers — per-event broadcast logic invoked inside the   */
/* main sim loop. Each takes the live event by const pointer.          */
/* ================================================================== */

static void srv_on_outpost_placed(const sim_event_t *ev) {
    int slot = ev->outpost_placed.slot;
    uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
    int id_len = serialize_station_identity(id_buf, slot, &world.stations[slot]);
    broadcast(id_buf, (size_t)id_len);
    station_identity_dirty[slot] = true;
    station_econ_dirty = true;
    contracts_dirty = true;
}

/* SELL / REPAIR / UPGRADE / DOCK / LAUNCH all need the player's ship +
 * current-station record pushed immediately so cargo / credits / hull /
 * dock status don't sit stale for the SHIP_TICK_MS window. */
static void srv_on_player_state_change(const sim_event_t *ev) {
    int pid = ev->player_id;
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->connected || !sp->conn) {
        station_econ_dirty = true;
        contracts_dirty = true;
        return;
    }
    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->conn, buf, (size_t)len);

    int st_idx = sp->current_station;
    if (st_idx >= 0 && st_idx < MAX_STATIONS) {
        uint8_t sbuf[2 + STATION_RECORD_SIZE];
        sbuf[0] = NET_MSG_WORLD_STATIONS;
        sbuf[1] = 1;
        uint8_t *p = &sbuf[2];
        p[0] = (uint8_t)st_idx;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            write_f32_le(&p[1 + c * 4], world.stations[st_idx]._inventory_cache[c]);
        ws_send(sp->conn, sbuf, (size_t)(2 + STATION_RECORD_SIZE));
    }
    station_econ_dirty = true;
    contracts_dirty = true;
}

/* SIM_EVENT_DEATH: highscore submission + per-player death packet
 * (carries pos/vel/stats so the client cinematic anchors at the
 * wreckage before the server respawn moves the ship) + fresh ship
 * state so the post-respawn hull/dock is visible immediately. */
static void srv_on_death(const sim_event_t *ev) {
    int pid = ev->player_id;
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->connected) return;

    const char *cs = sp->callsign;
    bool qualified = highscore_submit(&highscores, cs, ev->death.credits_earned);
    printf("[server] death pid=%d cs=%s earned=%.0f cr -> %s (top=%d)\n",
           pid, cs[0] ? cs : "?", ev->death.credits_earned,
           qualified ? "qualified" : "skipped", highscores.count);
    if (qualified) highscores_dirty = true;

    if (!sp->conn) return;

    /* Death packet: [type:1][pid:1][px:f32][py:f32][vx:f32][vy:f32]
     * [ang:f32][ore:f32][earned:f32][spent:f32][asteroids:f32]
     * [respawn_station:u8][respawn_fee:f32] = 43 bytes */
    uint8_t msg[43];
    msg[0] = NET_MSG_DEATH;
    msg[1] = (uint8_t)pid;
    write_f32_le(&msg[2],  ev->death.pos_x);
    write_f32_le(&msg[6],  ev->death.pos_y);
    write_f32_le(&msg[10], ev->death.vel_x);
    write_f32_le(&msg[14], ev->death.vel_y);
    write_f32_le(&msg[18], ev->death.angle);
    write_f32_le(&msg[22], ev->death.ore_mined);
    write_f32_le(&msg[26], ev->death.credits_earned);
    write_f32_le(&msg[30], ev->death.credits_spent);
    write_f32_le(&msg[34], (float)ev->death.asteroids_fractured);
    msg[38] = ev->death.respawn_station;
    write_f32_le(&msg[39], ev->death.respawn_fee);
    ws_send(sp->conn, msg, sizeof(msg));

    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->conn, buf, (size_t)len);
}

static void srv_on_contract_complete(const sim_event_t *ev) {
    (void)ev;
    station_econ_dirty = true;
    contracts_dirty = true;
}

static void srv_on_hail_response(const sim_event_t *ev) {
    int pid = ev->player_id;
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->connected || !sp->conn) return;

    uint8_t msg[7];
    msg[0] = NET_MSG_HAIL_RESPONSE;
    msg[1] = (uint8_t)ev->hail_response.station;
    write_f32_le(&msg[2], ev->hail_response.credits);
    int ci = ev->hail_response.contract_index;
    msg[6] = (ci >= 0 && ci < MAX_CONTRACTS) ? (uint8_t)ci : 0xFF;
    ws_send(sp->conn, msg, sizeof(msg));

    contracts_dirty = true;
    /* Push fresh ship state so the credit bump is visible immediately. */
    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->conn, buf, (size_t)len);
}

/* OUTPOST_PLACED / OUTPOST_ACTIVATED / MODULE_ACTIVATED / SCAFFOLD_READY
 * all need station identity refreshed so the client sees updated
 * module / pending lists. */
static void srv_mark_all_stations_identity_dirty(void) {
    for (int s = 0; s < MAX_STATIONS; s++) station_identity_dirty[s] = true;
}

/* Fan a single sim event out to its per-type broadcaster(s). Multiple
 * "if" branches on event type (rather than a switch) so events that
 * fall into more than one bucket — OUTPOST_PLACED touches both
 * srv_on_outpost_placed AND the structure-event identity refresh —
 * all run. */
static void srv_dispatch_sim_event(const sim_event_t *ev) {
    if (ev->type == SIM_EVENT_OUTPOST_PLACED) srv_on_outpost_placed(ev);
    if (ev->type == SIM_EVENT_SELL ||
        ev->type == SIM_EVENT_REPAIR ||
        ev->type == SIM_EVENT_UPGRADE ||
        ev->type == SIM_EVENT_DOCK ||
        ev->type == SIM_EVENT_LAUNCH) {
        srv_on_player_state_change(ev);
    }
    if (ev->type == SIM_EVENT_DEATH)              srv_on_death(ev);
    if (ev->type == SIM_EVENT_CONTRACT_COMPLETE)  srv_on_contract_complete(ev);
    if (ev->type == SIM_EVENT_HAIL_RESPONSE)      srv_on_hail_response(ev);
    if (ev->type == SIM_EVENT_OUTPOST_PLACED ||
        ev->type == SIM_EVENT_OUTPOST_ACTIVATED ||
        ev->type == SIM_EVENT_MODULE_ACTIVATED ||
        ev->type == SIM_EVENT_SCAFFOLD_READY) {
        srv_mark_all_stations_identity_dirty();
    }
}

/* ================================================================== */
/* Bootstrap helpers — pulled out of main() for clarity.               */
/* ================================================================== */

/* Read PORT, SIGNAL_API_TOKEN, SIGNAL_ALLOWED_ORIGIN, SIGNAL_REQUIRE_API_TOKEN
 * env vars and stamp the listen URL + CORS headers. Returns false (and
 * caller should exit nonzero) if SIGNAL_REQUIRE_API_TOKEN is set without
 * a token. listen_url is sized by the caller. */
static bool read_env_config(char *listen_url, size_t listen_url_size) {
    const char *port = getenv("PORT");
    if (!port) port = "8080";
    api_token = getenv("SIGNAL_API_TOKEN");
    if (api_token && api_token[0] != '\0') {
        printf("[server] Station API enabled (token set)\n");
    } else {
        fprintf(stderr, "[WARN] SIGNAL_API_TOKEN is unset -- REST API will reject all requests\n");
        if (getenv("SIGNAL_REQUIRE_API_TOKEN")) {
            fprintf(stderr, "[FATAL] SIGNAL_REQUIRE_API_TOKEN set but no token provided\n");
            return false;
        }
    }
    internal_token = getenv("SIGNAL_INTERNAL_SHARED_KEY");
    if (internal_token && internal_token[0] != '\0') {
        printf("[server] Internal operator-post endpoint enabled (token set)\n");
    } else {
        fprintf(stderr, "[WARN] SIGNAL_INTERNAL_SHARED_KEY is unset -- /internal/v1/operator-post will reject all requests\n");
    }
    allowed_origin = getenv("SIGNAL_ALLOWED_ORIGIN");
    if (!allowed_origin) allowed_origin = "*";
    snprintf(api_headers, sizeof(api_headers),
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Cache-Control: no-store\r\n", allowed_origin);
    printf("[server] CORS origin: %s\n", allowed_origin);
    snprintf(listen_url, listen_url_size, "http://0.0.0.0:%s", port);
    return true;
}

static void ensure_persistence_dirs(void) {
#ifdef _WIN32
    _mkdir(PLAYER_SAVE_DIR);
    _mkdir(STATION_CATALOG_DIR);
#else
    mkdir(PLAYER_SAVE_DIR, 0755);
    mkdir(STATION_CATALOG_DIR, 0755);
#endif
    /* Layer A.4 of #479: ensure pubkey/ + legacy/ subdirs exist and any
     * existing top-level *.sav files (v39 and earlier layout) get moved
     * into legacy/ so the new path layout takes effect. Idempotent. */
    player_save_migrate_legacy_layout(PLAYER_SAVE_DIR);
}

/* Layered persistence (#314):
 *   1. world_reset() seeds starter stations + belt field
 *   2. Catalog overwrites identity for any persisted stations
 *   3. Session snapshot overlays economy state
 *   4. Rebuild derived structures (signal chain, station nav, hash chain) */
static void load_world_state(void) {
    world_reset(&world);

    int catalog_count = station_catalog_load_all(world.stations, MAX_STATIONS,
                                                  STATION_CATALOG_DIR);
    if (catalog_count > 0)
        printf("[server] loaded %d station(s) from catalog\n", catalog_count);

    if (world_load(&world, SAVE_PATH)) {
        printf("[server] loaded session from %s\n", SAVE_PATH);
    } else {
        printf("[server] no session save -- fresh economy\n");
        /* Stations are sovereign currency issuers; no seed pool. The
         * pool just tracks net issuance from genesis. */
        world_seed_station_manifests(&world);
    }

    /* Assign stable IDs to any stations loaded from v1 catalogs (id == 0). */
    if (world.next_station_id == 0) world.next_station_id = 1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_exists(&world.stations[i]) && world.stations[i].id == 0)
            world.stations[i].id = world.next_station_id++;
    }

    /* The station catalog format doesn't persist currency_name (yet)
     * and the catalog loader memsets the whole struct, so the defaults
     * set by world_reset() get wiped. Re-stamp the names for the three
     * starter stations whenever they come back empty. */
    static const char *defaults[3] = {
        "prospect vouchers", "kepler bonds", "helios credits",
    };
    for (int i = 0; i < 3 && i < MAX_STATIONS; i++) {
        if (!station_exists(&world.stations[i])) continue;
        if (world.stations[i].currency_name[0] == '\0') {
            snprintf(world.stations[i].currency_name,
                     sizeof(world.stations[i].currency_name),
                     "%s", defaults[i]);
            station_identity_dirty[i] = true;
        }
    }

    rebuild_signal_chain(&world);
    station_rebuild_all_nav(&world);
    for (int i = 0; i < MAX_STATIONS; i++) station_identity_dirty[i] = true;

    /* Replay the on-disk hash chain so the Network tab survives a
     * server restart and the chain links continue from where we left
     * off (no fork at the genesis block). */
    signal_chain_load(&world);

    highscore_load(&highscores, HIGHSCORE_PATH);
    if (highscores.count > 0)
        printf("[server] loaded %d highscore(s) from %s\n",
               highscores.count, HIGHSCORE_PATH);
}

/* Run as many fixed-step sim ticks as `sim_accum` covers, up to
 * MAX_SIM_STEPS, broadcasting per-event side effects after each tick.
 * Caller passes the running accumulator + the elapsed-since-last-call
 * seconds. Returns when steps run out or the accumulator is empty. */
static void run_sim_ticks(float *sim_accum, float elapsed) {
    *sim_accum += elapsed;
    int steps = 0;
    while (*sim_accum >= SIM_DT && steps < MAX_SIM_STEPS) {
        world_sim_step(&world, SIM_DT);
        for (int e = 0; e < world.events.count; e++)
            srv_dispatch_sim_event(&world.events.events[e]);
        if (world.events.count > 0) {
            uint8_t ebuf[2 + SIM_MAX_EVENTS * NET_EVENT_RECORD_SIZE];
            int elen = serialize_events(ebuf, &world.events);
            if (elen > 2) broadcast(ebuf, (size_t)elen);
        }
        broadcast_fracture_updates();
        *sim_accum -= SIM_DT;
        steps++;
    }
    if (*sim_accum > SIM_DT) *sim_accum = 0.0f; /* prevent spiral */
}

/* Tick down per-player grace timers and session-auth timeouts. */
static void tick_session_timers(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (sp->connected && sp->grace_period) {
            sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
            if (sp->grace_timer <= 0.0f) {
                sp->connected = false;
                sp->grace_period = false;
                uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                broadcast(leave_msg, 2);
                printf("[server] player %d grace expired, fully disconnected\n", i);
            }
        }
        /* Kick clients that never sent SESSION within the auth window. */
        if (sp->connected && !sp->session_ready && !sp->grace_period) {
            sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
            if (sp->grace_timer <= 0.0f) {
                printf("[server] player %d: session timeout, disconnecting\n", i);
                mg_ws_send(sp->conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                sp->connected = false;
                sp->conn = NULL;
                uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                broadcast(leave_msg, 2);
            }
        }
    }
}

/* WORLD_TICK_MS broadcast: dirty station identities + named-ingot
 * stockpiles + manifest summaries. */
static void broadcast_dirty_station_data(uint64_t now, uint64_t *last_station_identity_p) {
    if (now - *last_station_identity_p >= STATION_IDENTITY_FALLBACK_MS) {
        for (int s = 0; s < MAX_STATIONS; s++) station_identity_dirty[s] = true;
        *last_station_identity_p = now;
    }
    /* Re-broadcast dirty station identities only to players in signal range. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_identity_dirty[s]) continue;
        if (!station_exists(&world.stations[s])) continue;
        uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
        int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
        float sr_sq = world.stations[s].signal_range * world.stations[s].signal_range;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!world.players[p].connected || !world.players[p].conn) continue;
            if (v2_dist_sq(world.players[p].ship.pos, world.stations[s].pos) <= sr_sq)
                ws_send(world.players[p].conn, id_buf, (size_t)id_len);
        }
        station_identity_dirty[s] = false;
    }
    /* RATi v2: per-station named-ingot snapshot (derived from the
     * unified manifest) + per-(commodity, grade) manifest summary.
     * Smaller payload than identity (~3KB worst case) so we send to
     * everyone regardless of signal range — MARKET is global. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!world.stations[s].manifest_dirty) continue;
        if (!station_exists(&world.stations[s])) continue;
        uint8_t buf[STATION_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
        int len = serialize_station_ingots(buf, s, &world.stations[s]);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!world.players[p].connected || !world.players[p].conn) continue;
            ws_send(world.players[p].conn, buf, (size_t)len);
        }
        uint8_t mbuf[STATION_MANIFEST_HEADER +
                     COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY];
        int mlen = serialize_station_manifest(mbuf, s, &world.stations[s]);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!world.players[p].connected || !world.players[p].conn) continue;
            ws_send(world.players[p].conn, mbuf, (size_t)mlen);
        }
        world.stations[s].manifest_dirty = false;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Line-buffer stdout and unbuffer stderr so `docker compose logs`
     * sees server output in real time. Without this, fully-buffered
     * stdout holds [server] printf lines until a 4KB page fills. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    char listen_url[64];
    if (!read_env_config(listen_url, sizeof(listen_url))) return 1;

    ensure_persistence_dirs();
    load_world_state();

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, listen_url, ev_handler, NULL);
#ifdef GIT_HASH
    printf("[server] SIGNAL alpha %s on %s\n", GIT_HASH, listen_url);
#else
    printf("[server] SIGNAL alpha on %s\n", listen_url);
#endif
    printf("[server] ALPHA BUILD -- world may reset without notice\n");

    uint64_t last_sim = 0, last_state = 0, last_world = 0, last_ship = 0, last_save = 0;
    uint64_t last_econ_dirty = 0;
    float sim_accum = 0.0f;

    while (running) {
        mg_mgr_poll(&mgr, 1);
        uint64_t now = mg_millis();

        if (now - last_sim >= SIM_TICK_MS) {
            float elapsed = (float)(now - last_sim) / 1000.0f;
            last_sim = now;
            run_sim_ticks(&sim_accum, elapsed);
            /* Mark econ dirty every ~1s as fallback for production changes. */
            if (now - last_econ_dirty >= 1000) {
                station_econ_dirty = true;
                contracts_dirty = true;
                last_econ_dirty = now;
            }
        }
        tick_session_timers();
        if (now - last_state >= STATE_TICK_MS) {
            broadcast_player_states();
            last_state = now;
        }
        if (now - last_world >= WORLD_TICK_MS) {
            broadcast_world();
            broadcast_dirty_station_data(now, &last_station_identity);
            last_world = now;
        }
        if (now - last_ship >= SHIP_TICK_MS) {
            broadcast_ship_states();
            last_ship = now;
        }
        if (highscores_dirty) {
            broadcast_highscores();
            (void)highscore_save(&highscores, HIGHSCORE_PATH);
            highscores_dirty = false;
        }
        if (now - last_save >= AUTOSAVE_MS) {
            station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
            world_save(&world, SAVE_PATH);
            last_save = now;
        }
    }

    mg_mgr_free(&mgr);
    station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
    world_save(&world, SAVE_PATH);
    printf("[server] world saved\n");
    printf("[server] shutdown\n");
    return 0;
}
