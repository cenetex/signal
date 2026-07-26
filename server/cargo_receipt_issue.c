/*
 * cargo_receipt_issue.c -- Server-side cargo_receipt_t issuance + emit.
 *
 * See cargo_receipt_issue.h for the public contract. This file glues
 * shared/cargo_receipt.h (wire format + verify) to server-only
 * primitives: station_authority signing and chain_log_emit.
 */
#include "cargo_receipt_issue.h"

#include "game_sim.h"
#include "manifest.h"
#include "station_authority.h"

#include <stdio.h>
#include <string.h>

bool cargo_receipt_issue(const station_t *s,
                         uint64_t epoch,
                         uint64_t event_id,
                         const uint8_t cargo_pub[32],
                         const uint8_t recipient_pubkey[32],
                         const uint8_t prev_receipt_hash[32],
                         cargo_receipt_t *out) {
    if (!s || !out || !cargo_pub || !recipient_pubkey || !prev_receipt_hash)
        return false;
    static const uint8_t zero32[32] = {0};
    if (memcmp(s->station_pubkey, zero32, 32) == 0) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->cargo_pub, cargo_pub, 32);
    memcpy(out->authoring_station, s->station_pubkey, 32);
    memcpy(out->recipient_pubkey, recipient_pubkey, 32);
    out->event_id = event_id;
    out->epoch = epoch;
    memcpy(out->prev_receipt_hash, prev_receipt_hash, 32);

    uint8_t blob[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(out, blob);
    station_sign(s, blob, sizeof(blob), out->signature);
    return true;
}

uint64_t cargo_receipt_emit_transfer(world_t *w, station_t *s,
                                     const uint8_t from_pubkey[32],
                                     const uint8_t to_pubkey[32],
                                     const uint8_t cargo_pub[32],
                                     uint8_t cargo_kind,
                                     const cargo_receipt_chain_t *incoming_chain,
                                     cargo_receipt_t *out_receipt) {
    if (!s || !out_receipt) return 0;
    cargo_receipt_transfer_link_t link =
        cargo_receipt_prepare_transfer_link(s, cargo_pub, incoming_chain);
    if (link.status != CARGO_RECEIPT_TRANSFER_LINK_READY) {
        memset(out_receipt, 0, sizeof(*out_receipt));
        return 0;
    }
    /* Wire-stable EVT_TRANSFER payload — typedef'd in chain_log.h so
     * the on-disk byte format has a single source of truth across
     * every emit site. */
    chain_payload_transfer_t xfer = {0};
    if (from_pubkey)   memcpy(xfer.from_pubkey, from_pubkey, 32);
    if (to_pubkey)     memcpy(xfer.to_pubkey,   to_pubkey,   32);
    if (cargo_pub)     memcpy(xfer.cargo_pub,   cargo_pub,   32);
    xfer.kind = cargo_kind;

    uint64_t event_id = chain_log_emit(w, s, CHAIN_EVT_TRANSFER,
                                       &xfer, (uint16_t)sizeof(xfer));
    if (event_id == 0) {
        memset(out_receipt, 0, sizeof(*out_receipt));
        return 0;
    }
    /* Epoch in ticks — same convention chain_log_emit used. */
    uint64_t epoch_ticks = w ? (uint64_t)(w->time * 120.0) : 0;
    if (!cargo_receipt_issue(s, epoch_ticks, event_id, cargo_pub,
                             to_pubkey ? to_pubkey : (const uint8_t[32]){0},
                             link.prev_receipt_hash, out_receipt))
        return 0;
    return event_id;
}

cargo_receipt_origin_resolve_status_t cargo_receipt_resolve_local_origin(
    const station_t *station,
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof) {
    static const uint8_t zero32[32] = {0};
    if (out_proof) memset(out_proof, 0, sizeof(*out_proof));
    if (!station || !cargo_pub || !out_proof ||
        memcmp(cargo_pub, zero32, sizeof(zero32)) == 0) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS;
    }

    char path[256];
    if (!chain_log_path_for(station->station_pubkey, path, sizeof(path)))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    FILE *history = fopen(path, "rb");
    if (!history)
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    fclose(history);

    chain_log_verify_report_t report;
    if (!chain_log_verify_station(station, NULL, NULL, &report))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;

    chain_cargo_transform_t transform;
    if (!chain_log_find_cargo_transform(station, cargo_pub, &transform))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;

    switch ((chain_event_type_t)transform.type) {
        case CHAIN_EVT_SMELT:
            out_proof->event_type = CARGO_RECEIPT_ORIGIN_EVENT_SMELT;
            memcpy(out_proof->output_cargo_pub, transform.smelt.ingot_pub,
                   sizeof(out_proof->output_cargo_pub));
            break;
        case CHAIN_EVT_CRAFT:
            out_proof->event_type = CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
            memcpy(out_proof->output_cargo_pub, transform.craft.output_pub,
                   sizeof(out_proof->output_cargo_pub));
            break;
        default:
            return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
    }
    out_proof->event_id = transform.event_id;
    out_proof->epoch = transform.epoch;
    memcpy(out_proof->event_hash, transform.header_hash,
           sizeof(out_proof->event_hash));
    memcpy(out_proof->authority, transform.authority,
           sizeof(out_proof->authority));
    return CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED;
}

const char *cargo_receipt_origin_resolve_status_name(
    cargo_receipt_origin_resolve_status_t status) {
    switch (status) {
        case CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED:
            return "not_attempted";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED:
            return "verified";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS:
            return "bad_arguments";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE:
            return "history_unavailable";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID:
            return "history_invalid";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND:
            return "transform_not_found";
        default:
            return "unknown";
    }
}

cargo_receipt_transfer_link_t cargo_receipt_prepare_transfer_link(
    const station_t *station,
    const uint8_t cargo_pub[32],
    const cargo_receipt_chain_t *incoming_chain) {
    cargo_receipt_transfer_link_t out = {
        .status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
        .chain_result = CARGO_RECEIPT_REJECT_EMPTY,
        .origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS,
    };
    if (!station || !cargo_pub) return out;

    if (incoming_chain && incoming_chain->len > 0) {
        if (incoming_chain->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
            out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN_FULL;
            return out;
        }
        out.chain_result = cargo_receipt_chain_verify(
            incoming_chain->links, incoming_chain->len, cargo_pub);
        if (out.chain_result != CARGO_RECEIPT_OK) {
            out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN;
            return out;
        }
        cargo_receipt_hash(
            &incoming_chain->links[incoming_chain->len - 1],
            out.prev_receipt_hash);
        out.origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED;
        out.status = CARGO_RECEIPT_TRANSFER_LINK_READY;
        return out;
    }

    cargo_receipt_origin_proof_t proof;
    out.origin_status = cargo_receipt_resolve_local_origin(
        station, cargo_pub, &proof);
    if (out.origin_status != CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN;
        return out;
    }
    memcpy(out.prev_receipt_hash, proof.event_hash,
           sizeof(out.prev_receipt_hash));
    out.chain_result = CARGO_RECEIPT_OK;
    out.status = CARGO_RECEIPT_TRANSFER_LINK_READY;
    return out;
}

static bool receipt_chain_prefix_matches(const cargo_receipt_chain_t *existing,
                                         const cargo_receipt_t *presented,
                                         uint8_t presented_len) {
    if (!existing || existing->len == 0) return true;
    if (!presented || presented_len < existing->len) return false;
    for (uint8_t i = 0; i < existing->len; i++) {
        if (memcmp(&existing->links[i], &presented[i],
                   sizeof(existing->links[i])) != 0) {
            return false;
        }
    }
    return true;
}

cargo_receipt_present_result_t cargo_receipt_present_to_ship(
    server_player_t *sp,
    const uint8_t cargo_pub[32],
    const cargo_receipt_t *chain,
    uint8_t chain_len) {
    static const uint8_t zero32[32] = {0};

    if (!sp || !cargo_pub || !chain || chain_len == 0 ||
        chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
        return CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS;
    }
    if (!sp->pubkey_set || memcmp(sp->pubkey, zero32, 32) == 0)
        return CARGO_RECEIPT_PRESENT_REJECT_NO_PLAYER_KEY;
    if (memcmp(cargo_pub, zero32, 32) == 0)
        return CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS;

    if (cargo_receipt_chain_verify(chain, chain_len, cargo_pub) !=
        CARGO_RECEIPT_OK) {
        return CARGO_RECEIPT_PRESENT_REJECT_VERIFY;
    }
    if (memcmp(chain[chain_len - 1].recipient_pubkey, sp->pubkey, 32) != 0)
        return CARGO_RECEIPT_PRESENT_REJECT_RECIPIENT;

    if (!ship_manifest_bootstrap(sp->ship))
        return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;

    int idx = manifest_find(&sp->ship->manifest, cargo_pub);
    if (idx < 0) return CARGO_RECEIPT_PRESENT_REJECT_NOT_CARRIED;

    ship_receipts_t *receipts = ship_get_receipts(sp->ship);
    if (!receipts) return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;
    if ((uint16_t)idx >= receipts->count) {
        if (!ship_receipts_reserve(receipts, sp->ship->manifest.count))
            return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;
        while (receipts->count < sp->ship->manifest.count) {
            if (!ship_receipts_push_empty(receipts))
                return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;
        }
    }

    cargo_receipt_chain_t *slot = &receipts->chains[idx];
    if (!receipt_chain_prefix_matches(slot, chain, chain_len))
        return CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH;

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->links, chain, (size_t)chain_len * sizeof(chain[0]));
    slot->len = chain_len;
    return CARGO_RECEIPT_PRESENT_OK;
}
