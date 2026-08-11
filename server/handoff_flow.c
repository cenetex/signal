#include "handoff_flow.h"

#include "manifest.h"
#include "station_util.h"

#include <string.h>

static bool station_ready_for_handoff(const station_t *st) {
    return st && station_is_active(st);
}

static int find_station_by_pubkey(const world_t *w, const uint8_t pubkey[32]) {
    if (!w || !pubkey) return -1;
    for (int i = 0; i < w->station_count && i < MAX_STATIONS; i++) {
        if (!station_ready_for_handoff(&w->stations[i])) continue;
        if (memcmp(w->stations[i].station_pubkey, pubkey, 32) == 0)
            return i;
    }
    return -1;
}

static bool handoff_ticket_hash_seen(const world_t *w, const uint8_t hash[32]) {
    if (!w || !hash) return false;
    uint16_t count = w->handoff_consumed_ticket_count;
    uint16_t cap = (uint16_t)(sizeof(w->handoff_consumed_ticket_hashes) /
                              sizeof(w->handoff_consumed_ticket_hashes[0]));
    if (count > cap) count = cap;
    for (uint16_t i = 0; i < count; i++) {
        if (memcmp(w->handoff_consumed_ticket_hashes[i], hash, 32) == 0)
            return true;
    }
    return false;
}

static void handoff_ticket_hash_remember(world_t *w, const uint8_t hash[32]) {
    if (!w || !hash) return;
    uint16_t cap = (uint16_t)(sizeof(w->handoff_consumed_ticket_hashes) /
                              sizeof(w->handoff_consumed_ticket_hashes[0]));
    uint16_t slot = 0;
    if (w->handoff_consumed_ticket_count < cap) {
        slot = w->handoff_consumed_ticket_count++;
    } else {
        slot = (uint16_t)(w->handoff_consumed_ticket_next % cap);
        w->handoff_consumed_ticket_next = (uint16_t)((slot + 1u) % cap);
    }
    memcpy(w->handoff_consumed_ticket_hashes[slot], hash, 32);
}

const char *handoff_flow_result_name(handoff_flow_result_t result) {
    switch (result) {
    case HANDOFF_FLOW_OK: return "ok";
    case HANDOFF_FLOW_REJECT_BAD_ARGS: return "bad-args";
    case HANDOFF_FLOW_REJECT_NO_PLAYER_KEY: return "no-player-key";
    case HANDOFF_FLOW_REJECT_SOURCE: return "source";
    case HANDOFF_FLOW_REJECT_DEST: return "dest";
    case HANDOFF_FLOW_REJECT_ISSUE: return "issue";
    case HANDOFF_FLOW_REJECT_VERIFY: return "verify";
    case HANDOFF_FLOW_REJECT_REPLAY: return "replay";
    case HANDOFF_FLOW_REJECT_HYDRATE: return "hydrate";
    default: return "unknown";
    }
}

bool handoff_issue_ticket_to_station(world_t *w, int player_idx,
                                     int source_station_idx,
                                     int dest_station_idx,
                                     uint64_t ttl_ticks,
                                     handoff_ticket_t *out) {
    if (!w || !out || player_idx < 0 || player_idx >= MAX_PLAYERS ||
        source_station_idx < 0 || source_station_idx >= MAX_STATIONS ||
        dest_station_idx < 0 || dest_station_idx >= MAX_STATIONS) {
        return false;
    }

    server_player_t *sp = &w->players[player_idx];
    station_t *src = &w->stations[source_station_idx];
    station_t *dst = &w->stations[dest_station_idx];
    if (!server_player_is_gameplay_ready(sp) ||
        !server_player_can_use_pubkey_persistence(sp) ||
        !station_ready_for_handoff(src) ||
        !station_ready_for_handoff(dst)) {
        return false;
    }

    if (ttl_ticks == 0) ttl_ticks = HANDOFF_TICKET_DEFAULT_TTL_TICKS;
    return handoff_ticket_issue_for_ship(
        src->station_pubkey, src->station_secret,
        dst->station_pubkey, sp->pubkey,
        src->id, dst->id,
        (uint64_t)w->tick,
        (uint64_t)w->tick + ttl_ticks,
        sp->ship, out);
}

handoff_flow_result_t handoff_accept_presented_ship(world_t *w, int player_idx,
                                                    const handoff_ticket_t *ticket,
                                                    const ship_t *presented_ship,
                                                    int *out_dest_station) {
    uint8_t ticket_hash[32];
    int source_idx = -1;
    int dest_idx = -1;
    handoff_ticket_result_t vr;

    if (out_dest_station) *out_dest_station = -1;
    if (!w || !ticket || !presented_ship ||
        player_idx < 0 || player_idx >= MAX_PLAYERS) {
        return HANDOFF_FLOW_REJECT_BAD_ARGS;
    }

    server_player_t *sp = &w->players[player_idx];
    if (!server_player_is_gameplay_ready(sp) ||
        !server_player_can_use_pubkey_persistence(sp))
        return HANDOFF_FLOW_REJECT_NO_PLAYER_KEY;

    source_idx = find_station_by_pubkey(w, ticket->source_authority);
    if (source_idx < 0 || ticket->source_zone != w->stations[source_idx].id)
        return HANDOFF_FLOW_REJECT_SOURCE;

    dest_idx = find_station_by_pubkey(w, ticket->dest_authority);
    if (dest_idx < 0 || ticket->dest_zone != w->stations[dest_idx].id)
        return HANDOFF_FLOW_REJECT_DEST;
    if (out_dest_station) *out_dest_station = dest_idx;

    vr = handoff_ticket_verify_for_ship(
        ticket, (uint64_t)w->tick,
        w->stations[source_idx].station_pubkey,
        w->stations[dest_idx].station_pubkey,
        sp->pubkey,
        presented_ship);
    if (vr != HANDOFF_TICKET_OK)
        return HANDOFF_FLOW_REJECT_VERIFY;

    handoff_ticket_hash(ticket, ticket_hash);
    if (handoff_ticket_hash_seen(w, ticket_hash))
        return HANDOFF_FLOW_REJECT_REPLAY;

    if (presented_ship != sp->ship && !ship_copy(sp->ship, presented_ship))
        return HANDOFF_FLOW_REJECT_HYDRATE;

    sp->docked = false;
    sp->docking_approach = false;
    sp->in_dock_range = false;
    sp->current_station = -1;
    sp->nearby_station = dest_idx;
    sp->replication->force_authoritative_resync = true;
    handoff_ticket_hash_remember(w, ticket_hash);
    return HANDOFF_FLOW_OK;
}
